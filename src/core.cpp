/// @file src/core.cpp
/// @brief Core implementation — lifecycle, plugin wiring, config bridge.

#include "core.hpp"

#include <boost/asio.hpp>
#include <fmt/core.h>

#include "config.hpp"
#include "logger.hpp"
#include "signals.hpp"
#include "cm/connectionManager.hpp"
#include "pm/pluginManager.hpp"
#include "util.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>

namespace gn {

namespace fs   = std::filesystem;
namespace asio = boost::asio;

// ── Impl ──────────────────────────────────────────────────────────────────────

struct Core::Impl {
    Config  owned_config_;   // used when external config is null
    Config* config_;         // always valid — points to owned_ or external

    std::unique_ptr<asio::io_context>                        ioc;
    asio::executor_work_guard<asio::io_context::executor_type> work;
    std::unique_ptr<SignalBus>                               bus;

    NodeIdentity                       identity;
    std::unique_ptr<ConnectionManager> cm;
    host_api_t                         host_api{};
    std::unique_ptr<PluginManager>     pm;

    std::vector<std::thread> io_threads;
    std::atomic<bool>        running{false};

    std::unique_ptr<asio::steady_timer> heartbeat_timer;

    explicit Impl(Config* ext_config)
        : owned_config_(true)  // defaults-only
        , config_(ext_config ? ext_config : &owned_config_)
        , ioc (std::make_unique<asio::io_context>())
        , work(asio::make_work_guard(*ioc))
        , bus (std::make_unique<SignalBus>(*ioc))
    {}
};

// ── Construction ──────────────────────────────────────────────────────────────

Core::Core(Config* config) : impl_(std::make_unique<Impl>(config)) {
    auto& d = *impl_;
    auto& cfg = *d.config_;

    // Logger
    Logger::set_log_level(cfg.logging.level);
    if (!cfg.logging.file.empty()) {
        Logger::set_log_file(cfg.logging.file);
        Logger::set_max_size(static_cast<size_t>(cfg.logging.max_size));
        Logger::set_max_files(cfg.logging.max_files);
    }

    // Identity
    Config::Identity id_cfg = cfg.identity;
    id_cfg.dir = expand_home(id_cfg.dir);
    d.identity = NodeIdentity::load_or_generate(id_cfg);

    // ConnectionManager
    LOG_TRACE("Core: identity loaded, creating CM");
    d.cm = std::make_unique<ConnectionManager>(*d.bus, d.identity, d.config_);
    d.cm->fill_host_api(&d.host_api);
    d.host_api.internal_logger = static_cast<void*>(Logger::get().get());

    // Plugins
    d.pm = std::make_unique<PluginManager>(&d.host_api, cfg.plugins.base_dir);

    d.pm->load_static_plugins();

    if (cfg.plugins.auto_load) {
        LOG_TRACE("Core: loading plugins from '{}'", cfg.plugins.base_dir);
        if (!cfg.plugins.base_dir.empty()) d.pm->load_all_plugins();

        // Extra plugin dirs (semicolon-separated)
        if (!cfg.plugins.extra_dirs.empty()) {
            auto extra = cfg.plugins.extra_dirs;
            std::istringstream ss(extra);
            std::string dir;
            while (std::getline(ss, dir, ';')) {
                if (dir.empty() || !fs::is_directory(dir)) continue;
                for (auto& e : fs::directory_iterator(dir)) {
                    auto ext = e.path().extension();
                    if (ext == ".so" || ext == ".dylib" || ext == ".dll") {
                        if (auto r = d.pm->load_plugin(e.path()); !r)
                            LOG_WARN("plugin load: {} — {}", e.path().string(), r.error());
                    }
                }
            }
        }

        size_t conn_count = 0, hdlr_count = 0;
        for (auto* c : d.pm->get_active_connectors()) {
            char scheme[64]{};
            if (c->get_scheme) c->get_scheme(c->connector_ctx, scheme, sizeof(scheme));
            if (scheme[0]) { d.cm->register_connector(scheme, c); ++conn_count; }
        }
        for (auto* h : d.pm->get_active_handlers()) {
            d.cm->register_handler(h); ++hdlr_count;
        }
        LOG_TRACE("Core: registered {} connectors, {} handlers", conn_count, hdlr_count);
    }

    LOG_INFO("Core ready — user={}", d.identity.user_pubkey_hex().substr(0, 12));
}

Core::~Core() {
    stop();
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────

void Core::run() {
    run_async();
    for (auto& t : impl_->io_threads) t.join();
    impl_->io_threads.clear();
}

void Core::start_heartbeat_timer() {
    auto& d = *impl_;
    d.heartbeat_timer = std::make_unique<asio::steady_timer>(*d.ioc);

    auto schedule = [this](auto&& self) -> void {
        auto& dd = *impl_;
        dd.heartbeat_timer->expires_after(std::chrono::seconds(30));
        dd.heartbeat_timer->async_wait([this, self](const boost::system::error_code& ec) {
            if (ec) return;  // timer cancelled (shutdown)
            impl_->cm->check_heartbeat_timeouts();
            impl_->cm->cleanup_stale_pending();
            self(self);
        });
    };
    schedule(schedule);
}

void Core::run_async(int threads) {
    auto& d = *impl_;
    if (d.running.exchange(true)) return;
    LOG_TRACE("Core::run_async threads={}", threads);

    start_heartbeat_timer();

    int n = threads > 0 ? threads : d.config_->core.io_threads;
    if (n <= 0) n = std::max(2, (int)std::thread::hardware_concurrency());
    d.io_threads.reserve((size_t)n);
    for (int i = 0; i < n; ++i)
        d.io_threads.emplace_back([this]{ impl_->ioc->run(); });
}

void Core::stop() {
    LOG_TRACE("Core::stop");
    auto& d = *impl_;
    if (!d.running.exchange(false)) return;
    if (d.heartbeat_timer) d.heartbeat_timer->cancel();
    d.cm->shutdown();
    d.work.reset();
    d.ioc->stop();
    for (auto& t : d.io_threads) if (t.joinable()) t.join();
    d.pm->unload_all();   // после join — нет callbacks in flight
    d.io_threads.clear();
    LOG_INFO("Core stopped.");
}

bool Core::is_running() const noexcept {
    return impl_->running.load(std::memory_order_relaxed);
}

// ── Network ───────────────────────────────────────────────────────────────────

bool Core::send(std::string_view uri, uint32_t t, std::span<const uint8_t> p) {
    return impl_->cm->send(uri, t, p);
}

bool Core::send(conn_id_t id, uint32_t t, std::span<const uint8_t> p) {
    return impl_->cm->send(id, t, p);
}

void Core::broadcast(uint32_t t, std::span<const uint8_t> p) {
    impl_->cm->broadcast(t, p);
}

void Core::connect(std::string_view uri) { impl_->cm->connect(uri); }
void Core::disconnect(conn_id_t id)      { impl_->cm->disconnect(id); }
void Core::close_now(conn_id_t id)       { impl_->cm->close_now(id); }

// ── Key management ────────────────────────────────────────────────────────────

bool Core::rekey_session(conn_id_t id) {
    return impl_->cm->rekey_session(id);
}
void Core::rotate_identity_keys() {
    auto& cfg = *impl_->config_;
    Config::Identity id_cfg = cfg.identity;
    id_cfg.dir = expand_home(id_cfg.dir);
    impl_->cm->rotate_identity_keys(id_cfg);
}

// ── Peer info ─────────────────────────────────────────────────────────────────

std::vector<uint8_t> Core::peer_pubkey(conn_id_t id) const {
    return impl_->cm->get_peer_pubkey(id).value_or(std::vector<uint8_t>{}); }
std::string Core::peer_pubkey_hex(conn_id_t id) const {
    return impl_->cm->get_peer_pubkey_hex(id); }
std::optional<endpoint_t> Core::peer_endpoint(conn_id_t id) const {
    return impl_->cm->get_peer_endpoint(id); }

// ── Subscriptions ─────────────────────────────────────────────────────────────

uint64_t Core::subscribe(uint32_t t, std::string_view n, PacketHandler cb, uint8_t p) {
    return impl_->bus->subscribe(t, n, std::move(cb), p); }
void Core::subscribe_wildcard(std::string_view n, PacketHandler cb, uint8_t p) {
    impl_->bus->subscribe_wildcard(n, std::move(cb), p); }
void Core::unsubscribe(uint64_t sub_id) {
    impl_->bus->unsubscribe(sub_id); }

// ── Identity ──────────────────────────────────────────────────────────────────

std::string Core::user_pubkey_hex()   const { return impl_->identity.user_pubkey_hex(); }
std::string Core::device_pubkey_hex() const { return impl_->identity.device_pubkey_hex(); }

// ── Stats ─────────────────────────────────────────────────────────────────────

StatsSnapshot Core::stats_snapshot() const noexcept {
    return impl_->bus->stats_snapshot(); }
size_t Core::connection_count() const noexcept {
    return impl_->cm->connection_count(); }
std::vector<std::string> Core::active_uris() const {
    return impl_->cm->get_active_uris(); }
std::vector<conn_id_t> Core::active_conn_ids() const {
    return impl_->cm->get_active_conn_ids(); }
std::string Core::dump_connections() const {
    return impl_->cm->dump_connections(); }

// ── Plugin info ───────────────────────────────────────────────────────────

size_t Core::handler_count() const noexcept {
    return impl_->pm->get_enabled_handler_count(); }
size_t Core::connector_count() const noexcept {
    return impl_->pm->get_enabled_connector_count(); }

// ── Config ────────────────────────────────────────────────────────────────────

bool Core::reload_config() {
    LOG_TRACE("Core::reload_config");
    auto& cfg = *impl_->config_;
    if (!cfg.reload()) return false;
    Logger::set_log_level(cfg.logging.level);
    LOG_INFO("Config reloaded.");
    return true;
}

// ── Internal ─────────────────────────────────────────────────────────────────

ConnectionManager& Core::cm()  noexcept { return *impl_->cm; }
PluginManager&     Core::pm()  noexcept { return *impl_->pm; }
SignalBus&         Core::bus() noexcept { return *impl_->bus; }

} // namespace gn
