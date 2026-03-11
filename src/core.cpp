/// @file src/core.cpp
/// @brief gn::Core implementation — ALL heavy includes live here only.
///
/// Rule: every include that would be toxic in a public header goes here.
/// The header sees only forward declarations.

// ── Own header ──────────────────────────────────────────────────────────────
#include "core.hpp"

// ── Heavy deps — NEVER in core.hpp ──────────────────────────────────────────
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

namespace fs = std::filesystem;
namespace asio = boost::asio;

// ─── Impl ─────────────────────────────────────────────────────────────────────

struct Core::Impl {
    CoreConfig cfg;

    Config                                config;
    std::unique_ptr<asio::io_context>     ioc;
    asio::executor_work_guard<asio::io_context::executor_type> work;
    std::unique_ptr<SignalBus>            bus;
    NodeIdentity                          identity;
    std::unique_ptr<ConnectionManager>    cm;
    host_api_t                            host_api{};
    std::unique_ptr<PluginManager>        pm;
    std::vector<std::thread>              io_threads;
    std::atomic<bool>                     running{false};

    explicit Impl(CoreConfig c)
        : cfg(std::move(c))
        , ioc(std::make_unique<asio::io_context>())
        , work(asio::make_work_guard(*ioc))
        , bus(std::make_unique<SignalBus>(*ioc))
    {}
};

// ─── Helpers ─────────────────────────────────────────────────────────────────

static fs::path expand_home(const fs::path& p) {
    std::string s = p.string();
    if (!s.starts_with("~/")) return p;
    const char* h = std::getenv("HOME");
#if defined(_WIN32)
    if (!h) h = std::getenv("USERPROFILE");
#endif
    return fs::path(h ? h : ".") / s.substr(2);
}

// ─── Core ─────────────────────────────────────────────────────────────────────

Core::Core(CoreConfig cfg) : impl_(std::make_unique<Impl>(std::move(cfg))) {
    auto& d = *impl_;

    // 1. Logger (before any LOG_* call)
    Logger::log_level = d.cfg.logging.level;
    if (!d.cfg.logging.file.empty()) {
        Logger::log_file  = d.cfg.logging.file;
        Logger::max_size  = d.cfg.logging.max_size;
        Logger::max_files = d.cfg.logging.max_files;
    }

    // 2. Optional JSON config
    if (!d.cfg.config_file.empty() && fs::exists(d.cfg.config_file))
        d.config.load_from_file(d.cfg.config_file);

    // 3. Identity
    IdentityConfig id_cfg;
    id_cfg.dir            = expand_home(d.cfg.identity.dir);
    id_cfg.ssh_key_path   = d.cfg.identity.ssh_key_path;
    id_cfg.use_machine_id = d.cfg.identity.use_machine_id;
    d.identity = NodeIdentity::load_or_generate(id_cfg);

    // 4. ConnectionManager
    d.cm = std::make_unique<ConnectionManager>(*d.bus, d.identity);

    // 5. host_api (binds logger + all cm callbacks)
    d.cm->fill_host_api(&d.host_api);
    d.host_api.internal_logger = static_cast<void*>(Logger::get().get());

    // 6. PluginManager
    fs::path base_dir;
    if (!d.cfg.plugins.dirs.empty())
        base_dir = d.cfg.plugins.dirs.front();
    d.pm = std::make_unique<PluginManager>(&d.host_api, base_dir);

    // 7. Auto-load + wire plugins
    if (d.cfg.plugins.auto_load) {
        if (!base_dir.empty())
            d.pm->load_all_plugins();

        // Extra dirs beyond the first
        for (size_t i = 1; i < d.cfg.plugins.dirs.size(); ++i) {
            const auto& dir = d.cfg.plugins.dirs[i];
            if (!fs::is_directory(dir)) continue;
            for (auto& e : fs::directory_iterator(dir)) {
                auto ext = e.path().extension();
                if (ext == ".so" || ext == ".dylib" || ext == ".dll") {
                    if (auto r = d.pm->load_plugin(e.path()); !r)
                        LOG_WARN("plugin load failed: {} — {}", e.path().string(), r.error());
                }
            }
        }

        // Wire into CM
        for (auto* c : d.pm->get_active_connectors()) {
            char scheme[64] = {};
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

void Core::send(const char* uri, uint32_t msg_type, const void* payload, size_t size) {
    impl_->cm->send(uri, msg_type, payload, size);
}
void Core::send(const char* uri, uint32_t msg_type, std::string_view payload) {
    impl_->cm->send(uri, msg_type, payload.data(), payload.size());
}

// ── Subscriptions ─────────────────────────────────────────────────────────────

void Core::subscribe(uint32_t msg_type, std::string_view name, PacketHandler cb, uint8_t prio) {
    impl_->bus->subscribe(msg_type, name, std::move(cb), prio);
}
void Core::subscribe_wildcard(std::string_view name, PacketHandler cb, uint8_t prio) {
    impl_->bus->subscribe_wildcard(name, std::move(cb), prio);
}

// ── Identity ─────────────────────────────────────────────────────────────────

std::string Core::user_pubkey_hex()   const { return impl_->identity.user_pubkey_hex(); }
std::string Core::device_pubkey_hex() const { return impl_->identity.device_pubkey_hex(); }

// ── Stats ─────────────────────────────────────────────────────────────────────

size_t Core::connection_count() const noexcept { return impl_->cm->connection_count(); }
std::vector<std::string> Core::active_uris()  const { return impl_->cm->get_active_uris(); }

// ── Internal access ───────────────────────────────────────────────────────────

ConnectionManager& Core::cm()  noexcept { return *impl_->cm; }
PluginManager&     Core::pm()  noexcept { return *impl_->pm; }
SignalBus&         Core::bus() noexcept { return *impl_->bus; }

} // namespace gn
