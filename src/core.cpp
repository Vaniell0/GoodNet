/// @file src/core.cpp

#include "core.hpp"

#include <boost/asio.hpp>
#include <fmt/core.h>

#include "config.hpp"
#include "logger.hpp"
#include "signals.hpp"
#include "connectionManager.hpp"
#include "pluginManager.hpp"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <thread>
#include <vector>

namespace gn {

namespace fs   = std::filesystem;
namespace asio = boost::asio;

// ── Impl ──────────────────────────────────────────────────────────────────────

struct Core::Impl {
    CoreConfig cfg;
    Config     config;

    std::unique_ptr<asio::io_context>                        ioc;
    asio::executor_work_guard<asio::io_context::executor_type> work;
    std::unique_ptr<SignalBus>                               bus;

    NodeIdentity                       identity;
    std::unique_ptr<ConnectionManager> cm;
    host_api_t                         host_api{};
    std::unique_ptr<PluginManager>     pm;

    std::vector<std::thread> io_threads;
    std::atomic<bool>        running{false};

    explicit Impl(CoreConfig c)
        : cfg (std::move(c))
        , ioc (std::make_unique<asio::io_context>())
        , work(asio::make_work_guard(*ioc))
        , bus (std::make_unique<SignalBus>(*ioc))
    {}
};

// ── Helpers ───────────────────────────────────────────────────────────────────

static fs::path expand_home(const fs::path& p) {
    std::string s = p.string();
    if (!s.starts_with("~/")) return p;
    const char* h = std::getenv("HOME");
#if defined(_WIN32)
    if (!h) h = std::getenv("USERPROFILE");
#endif
    return fs::path(h ? h : ".") / s.substr(2);
}

// ── Construction ──────────────────────────────────────────────────────────────

Core::Core(CoreConfig cfg) : impl_(std::make_unique<Impl>(std::move(cfg))) {
    auto& d = *impl_;

    Logger::log_level = d.cfg.logging.level;
    if (!d.cfg.logging.file.empty()) {
        Logger::log_file  = d.cfg.logging.file;
        Logger::max_size  = d.cfg.logging.max_size;
        Logger::max_files = d.cfg.logging.max_files;
    }

    if (!d.cfg.config_file.empty() && fs::exists(d.cfg.config_file))
        d.config.load_from_file(d.cfg.config_file);

    IdentityConfig id_cfg{
        .dir            = expand_home(d.cfg.identity.dir),
        .ssh_key_path   = d.cfg.identity.ssh_key_path,
        .use_machine_id = d.cfg.identity.use_machine_id,
    };
    d.identity = NodeIdentity::load_or_generate(id_cfg);

    d.cm = std::make_unique<ConnectionManager>(*d.bus, d.identity);
    d.cm->fill_host_api(&d.host_api);
    d.host_api.internal_logger = static_cast<void*>(Logger::get().get());

    // Wire config_get to our Config instance (closure captures &d.config)
    d.host_api.config_get = [](void* ctx, const char* key, char* buf, size_t sz) -> int {
        auto* self = static_cast<Core::Impl*>(ctx);
        auto v = self->config.get<std::string>(key);
        if (!v) return -1;
        std::strncpy(buf, v->c_str(), sz - 1);
        buf[sz - 1] = '\0';
        return static_cast<int>(v->size());
    };
    // ctx for config_get is Impl*, not ConnectionManager*
    // We need a separate ctx slot — store Impl* in a custom field via lambda capture
    // by making a trampoline that uses the host_api ctx only for config:
    // (core.h's host_api_t uses api->ctx for CM; config_get gets impl_ via a
    //  static thread_local set before fill_host_api returns — simplest approach)
    // Alternatively, override after fill_host_api:
    struct ConfigBridge {
        static int get(void* ctx, const char* key, char* buf, size_t sz) {
            // ctx == Impl* — see below
            auto* impl = static_cast<Core::Impl*>(ctx);
            auto v = impl->config.get<std::string>(key);
            if (!v) return -1;
            std::strncpy(buf, v->c_str(), sz - 1); buf[sz - 1] = '\0';
            return static_cast<int>(v->size());
        }
    };
    // We repurpose a second host_api copy for plugins that need config.
    // CM fill_host_api sets ctx = CM*, so we patch config_get with a separate
    // static trampoline that stores impl_ in the closure below:
    static Core::Impl* s_impl_ptr = nullptr;
    s_impl_ptr = &d;
    d.host_api.config_get = [](void*, const char* key, char* buf, size_t sz) -> int {
        if (!s_impl_ptr) return -1;
        auto v = s_impl_ptr->config.get<std::string>(key);
        if (!v) return -1;
        std::strncpy(buf, v->c_str(), sz - 1); buf[sz - 1] = '\0';
        return static_cast<int>(v->size());
    };

    fs::path base_dir;
    if (!d.cfg.plugins.dirs.empty()) base_dir = d.cfg.plugins.dirs.front();
    d.pm = std::make_unique<PluginManager>(&d.host_api, base_dir);

    if (d.cfg.plugins.auto_load) {
        if (!base_dir.empty()) d.pm->load_all_plugins();

        for (size_t i = 1; i < d.cfg.plugins.dirs.size(); ++i) {
            const auto& dir = d.cfg.plugins.dirs[i];
            if (!fs::is_directory(dir)) continue;
            for (auto& e : fs::directory_iterator(dir)) {
                auto ext = e.path().extension();
                if (ext == ".so" || ext == ".dylib" || ext == ".dll") {
                    if (auto r = d.pm->load_plugin(e.path()); !r)
                        LOG_WARN("plugin load: {} — {}", e.path().string(), r.error());
                }
            }
        }

        for (auto* c : d.pm->get_active_connectors()) {
            char scheme[64]{};
            if (c->get_scheme) c->get_scheme(c->connector_ctx, scheme, sizeof(scheme));
            if (scheme[0]) d.cm->register_connector(scheme, c);
        }
        for (auto* h : d.pm->get_active_handlers())
            d.cm->register_handler(h);
    }

    LOG_INFO("Core ready — user={}", d.identity.user_pubkey_hex().substr(0, 12));
}

Core::~Core() { stop(); }

// ── Lifecycle ─────────────────────────────────────────────────────────────────

void Core::run() {
    run_async();
    for (auto& t : impl_->io_threads) t.join();
    impl_->io_threads.clear();
}

void Core::run_async(int threads) {
    auto& d = *impl_;
    if (d.running.exchange(true)) return;
    int n = threads > 0 ? threads : d.cfg.network.io_threads;
    if (n <= 0) n = std::max(2, (int)std::thread::hardware_concurrency());
    d.io_threads.reserve((size_t)n);
    for (int i = 0; i < n; ++i)
        d.io_threads.emplace_back([this]{ impl_->ioc->run(); });
}

void Core::stop() {
    auto& d = *impl_;
    if (!d.running.exchange(false)) return;
    d.pm->unload_all();
    d.cm->shutdown();
    d.work.reset();
    d.ioc->stop();
    for (auto& t : d.io_threads) if (t.joinable()) t.join();
    d.io_threads.clear();
    LOG_INFO("Core stopped.");
}

bool Core::is_running() const noexcept {
    return impl_->running.load(std::memory_order_relaxed);
}

// ── Network ───────────────────────────────────────────────────────────────────

bool Core::send(const char* uri, uint32_t t, const void* p, size_t s) {
    return impl_->cm->send(uri, t, p, s); }
bool Core::send(const char* uri, uint32_t t, std::string_view p) {
    return impl_->cm->send(uri, t,
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(p.data()), p.size())); }
bool Core::send(const char* uri, uint32_t t, std::span<const uint8_t> p) {
    return impl_->cm->send(uri, t, p); }

bool Core::send_to(conn_id_t id, uint32_t t, const void* p, size_t s) {
    return impl_->cm->send_on_conn(id, t, p, s); }
bool Core::send_to(conn_id_t id, uint32_t t, std::string_view p) {
    return impl_->cm->send_on_conn(id, t,
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(p.data()), p.size())); }
bool Core::send_to(conn_id_t id, uint32_t t, std::span<const uint8_t> p) {
    return impl_->cm->send_on_conn(id, t, p); }

void Core::broadcast(uint32_t t, const void* p, size_t s) {
    impl_->cm->broadcast(t, p, s); }
void Core::broadcast(uint32_t t, std::string_view p) {
    impl_->cm->broadcast(t,
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(p.data()), p.size())); }
void Core::broadcast(uint32_t t, std::span<const uint8_t> p) {
    impl_->cm->broadcast(t, p); }

void Core::connect(std::string_view uri) { impl_->cm->connect(uri); }
void Core::disconnect(conn_id_t id)      { impl_->cm->disconnect(id); }
void Core::close_now(conn_id_t id)       { impl_->cm->close_now(id); }

// ── Key management ────────────────────────────────────────────────────────────

bool Core::rekey_session(conn_id_t id) {
    return impl_->cm->rekey_session(id);
}
void Core::rotate_identity_keys() {
    IdentityConfig cfg{
        .dir            = expand_home(impl_->cfg.identity.dir),
        .ssh_key_path   = impl_->cfg.identity.ssh_key_path,
        .use_machine_id = impl_->cfg.identity.use_machine_id,
    };
    impl_->cm->rotate_identity_keys(cfg);
}

// ── Peer info ─────────────────────────────────────────────────────────────────

std::vector<uint8_t> Core::peer_pubkey(conn_id_t id) const {
    return impl_->cm->get_peer_pubkey(id).value_or(std::vector<uint8_t>{}); }
bool Core::peer_endpoint(conn_id_t id, endpoint_t& out) const {
    return impl_->cm->get_peer_endpoint(id, out); }

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

// ── Config ────────────────────────────────────────────────────────────────────

bool Core::reload_config() {
    auto& d = *impl_;
    if (!d.config.reload()) return false;
    if (auto lvl = d.config.get<std::string>("logging.level"))
        Logger::log_level = *lvl;
    LOG_INFO("Config reloaded.");
    return true;
}

// ── Internal ─────────────────────────────────────────────────────────────────

ConnectionManager& Core::cm()  noexcept { return *impl_->cm; }
PluginManager&     Core::pm()  noexcept { return *impl_->pm; }
SignalBus&         Core::bus() noexcept { return *impl_->bus; }

} // namespace gn
