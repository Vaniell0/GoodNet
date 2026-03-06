#include "pluginManager.hpp"
#include "connectionManager.hpp"
#include "logger.hpp"

namespace gn {

PluginManager::HandlerInfo::~HandlerInfo() {
    if (handler && handler->shutdown)
        handler->shutdown(handler->user_data);
}

PluginManager::ConnectorInfo::~ConnectorInfo() {
    if (ops && ops->shutdown)
        ops->shutdown(ops->connector_ctx);
}

PluginManager::PluginManager(host_api_t* api, fs::path plugins_base_dir)
    : host_api_(api), plugins_base_dir_(std::move(plugins_base_dir))
{
    LOG_INFO("PluginManager: base={}",
             plugins_base_dir_.empty() ? "(none)" : plugins_base_dir_.string());
}

PluginManager::~PluginManager() {
    unload_all();
}

std::expected<void, std::string> PluginManager::load_plugin(const fs::path& path) {
    if (!fs::exists(path))
        return std::unexpected(fmt::format("not found: {}", path.string()));

    if (auto v = verify_metadata(path); !v)
        return std::unexpected(v.error());

    auto lib_result = DynLib::open(path);
    if (!lib_result)
        return std::unexpected(fmt::format("dlopen '{}': {}",
                                            path.filename().string(), lib_result.error()));

    DynLib lib = std::move(*lib_result);

    // ── Handler ───────────────────────────────────────────────────────────────
    using handler_init_fn = int(*)(host_api_t*, handler_t**);
    auto h_sym = lib.symbol<handler_init_fn>("handler_init");

    if (h_sym) {
        auto info    = std::make_unique<HandlerInfo>();
        info->lib    = std::move(lib);
        info->api_c  = *host_api_;
        info->api_c.plugin_type      = PLUGIN_TYPE_HANDLER;
        info->api_c.internal_logger  = host_api_->internal_logger;

        handler_t* h = nullptr;
        if ((*h_sym)(&info->api_c, &h) != 0 || !h)
            return std::unexpected(
                fmt::format("handler_init() failed: {}", path.filename().string()));

        std::string name = (h->name && h->name[0]) ? h->name : path.stem().string();

        std::unique_lock lock(rw_mutex_);
        if (handlers_.contains(name))
            return std::unexpected(fmt::format("handler '{}' already loaded", name));

        info->handler = h;
        info->path    = path;
        info->name    = name;

        LOG_INFO("Handler loaded: '{}' [{}]", name, path.filename().string());
        handlers_[name] = std::move(info);
        return {};
    }

    // ── Connector ─────────────────────────────────────────────────────────────
    using connector_init_fn = int(*)(host_api_t*, connector_ops_t**);
    auto c_sym = lib.symbol<connector_init_fn>("connector_init");

    if (c_sym) {
        auto info   = std::make_unique<ConnectorInfo>();
        info->lib   = std::move(lib);
        info->api_c = *host_api_;
        info->api_c.plugin_type     = PLUGIN_TYPE_CONNECTOR;
        info->api_c.internal_logger = host_api_->internal_logger;

        connector_ops_t* ops = nullptr;
        if ((*c_sym)(&info->api_c, &ops) != 0 || !ops)
            return std::unexpected(
                fmt::format("connector_init() failed: {}", path.filename().string()));

        char buf[256] = {};
        if (ops->get_scheme) ops->get_scheme(ops->connector_ctx, buf, sizeof(buf));
        std::string scheme = buf;
        if (scheme.empty())
            return std::unexpected(
                fmt::format("connector '{}': get_scheme() empty", path.filename().string()));

        buf[0] = '\0';
        if (ops->get_name) ops->get_name(ops->connector_ctx, buf, sizeof(buf));
        std::string conn_name = buf[0] ? buf : path.stem().string();

        std::unique_lock lock(rw_mutex_);
        if (connectors_.contains(scheme))
            return std::unexpected(fmt::format("connector scheme '{}' already loaded", scheme));

        info->ops    = ops;
        info->path   = path;
        info->name   = std::move(conn_name);
        info->scheme = scheme;

        LOG_INFO("Connector loaded: '{}' scheme='{}' [{}]",
                 info->name, scheme, path.filename().string());
        connectors_[scheme] = std::move(info);
        return {};
    }

    return std::unexpected(fmt::format(
        "'{}' exports neither handler_init nor connector_init",
        path.filename().string()));
}

void PluginManager::load_all_plugins() {
    if (plugins_base_dir_.empty()) {
        LOG_WARN("PluginManager: plugins_base_dir not set, skipping scan");
        return;
    }

    const std::array<fs::path, 2> subdirs = {
        plugins_base_dir_ / "handlers",
        plugins_base_dir_ / "connectors"
    };

    for (const auto& dir : subdirs) {
        if (!fs::exists(dir)) {
            LOG_DEBUG("Plugin dir not found: {}", dir.string());
            continue;
        }
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != DYNLIB_EXT) continue;
            auto result = load_plugin(entry.path());
            if (!result)
                LOG_WARN("Skip '{}': {}", entry.path().filename().string(), result.error());
        }
    }
}

void PluginManager::unload_all() {
    std::unique_lock lock(rw_mutex_);
    handlers_.clear();
    connectors_.clear();
    LOG_INFO("PluginManager: all plugins unloaded");
}

} // namespace gn
