#include "pluginManager.hpp"
#include "connectionManager.hpp"
#include "logger.hpp"
#include "static_registry.hpp"

namespace gn {

HandlerInfo::~HandlerInfo() {
    if (handler && handler->shutdown)
        handler->shutdown(handler->user_data);
}

HandlerInfo::HandlerInfo(HandlerInfo&& o) noexcept
    : lib(std::move(o.lib)), handler(o.handler), api(o.api),
      path(std::move(o.path)), name(std::move(o.name)),
      enabled(o.enabled.load(std::memory_order_relaxed))
{ o.handler = nullptr; }

HandlerInfo& HandlerInfo::operator=(HandlerInfo&& o) noexcept {
    if (this != &o) {
        lib     = std::move(o.lib);
        handler = o.handler; o.handler = nullptr;
        api     = o.api;
        path    = std::move(o.path);
        name    = std::move(o.name);
        enabled.store(o.enabled.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
    return *this;
}

ConnectorInfo::~ConnectorInfo() {
    if (ops && ops->shutdown)
        ops->shutdown(ops->connector_ctx);
}

ConnectorInfo::ConnectorInfo(ConnectorInfo&& o) noexcept
    : lib(std::move(o.lib)), ops(o.ops), api(o.api),
      path(std::move(o.path)), name(std::move(o.name)),
      scheme(std::move(o.scheme)),
      enabled(o.enabled.load(std::memory_order_relaxed))
{ o.ops = nullptr; }

ConnectorInfo& ConnectorInfo::operator=(ConnectorInfo&& o) noexcept {
    if (this != &o) {
        lib    = std::move(o.lib);
        ops    = o.ops; o.ops = nullptr;
        api    = o.api;
        path   = std::move(o.path);
        name   = std::move(o.name);
        scheme = std::move(o.scheme);
        enabled.store(o.enabled.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
    return *this;
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
        info->api  = *host_api_;
        info->api.plugin_type      = PLUGIN_TYPE_HANDLER;
        info->api.internal_logger  = host_api_->internal_logger;

        handler_t* h = nullptr;
        if ((*h_sym)(&info->api, &h) != 0 || !h)
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
        info->api = *host_api_;
        info->api.plugin_type     = PLUGIN_TYPE_CONNECTOR;
        info->api.internal_logger = host_api_->internal_logger;

        connector_ops_t* ops = nullptr;
        if ((*c_sym)(&info->api, &ops) != 0 || !ops)
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
    if (plugins_base_dir_.empty()) return;

    // Рекурсивный обход всех файлов в папке plugins
    for (const auto& entry : fs::recursive_directory_iterator(plugins_base_dir_)) {
        if (!entry.is_regular_file()) continue;
        
        auto path = entry.path();
        // Загружаем только бинарники, игнорируем .json и прочее
        if (path.extension() == DYNLIB_EXT) {
            auto result = load_plugin(path);
            if (!result) {
                // Если это не плагин (нет нужных символов) - просто идем дальше
                LOG_DEBUG("File {} is not a valid plugin: {}", path.filename().string(), result.error());
            }
        }
    }
}

void PluginManager::load_static_plugins() {
    auto& registry = gn::static_plugin_registry();
    if (registry.empty()) return;

    LOG_INFO("PluginManager: loading {} static plugin(s)", registry.size());

    for (auto& entry : registry) {
        // ── Handler ──────────────────────────────────────────────────────────
        if (entry.handler_init) {
            auto info   = std::make_unique<HandlerInfo>();
            info->api   = *host_api_;
            info->api.plugin_type     = PLUGIN_TYPE_HANDLER;
            info->api.internal_logger = host_api_->internal_logger;

            handler_t* h = nullptr;
            if (entry.handler_init(&info->api, &h) != 0 || !h) {
                LOG_WARN("Static handler '{}' init failed", entry.name);
                continue;
            }

            std::string name = (h->name && h->name[0]) ? h->name : entry.name;

            std::unique_lock lock(rw_mutex_);
            if (handlers_.contains(name)) {
                LOG_WARN("Static handler '{}' already loaded, skipping", name);
                continue;
            }

            info->handler = h;
            info->name    = name;
            LOG_INFO("Static handler loaded: '{}'", name);
            handlers_[name] = std::move(info);
            continue;
        }

        // ── Connector ────────────────────────────────────────────────────────
        if (entry.connector_init) {
            auto info   = std::make_unique<ConnectorInfo>();
            info->api   = *host_api_;
            info->api.plugin_type     = PLUGIN_TYPE_CONNECTOR;
            info->api.internal_logger = host_api_->internal_logger;

            connector_ops_t* ops = nullptr;
            if (entry.connector_init(&info->api, &ops) != 0 || !ops) {
                LOG_WARN("Static connector '{}' init failed", entry.name);
                continue;
            }

            char buf[256] = {};
            if (ops->get_scheme) ops->get_scheme(ops->connector_ctx, buf, sizeof(buf));
            std::string scheme = buf;
            if (scheme.empty()) {
                LOG_WARN("Static connector '{}': get_scheme() empty", entry.name);
                continue;
            }

            buf[0] = '\0';
            if (ops->get_name) ops->get_name(ops->connector_ctx, buf, sizeof(buf));
            std::string conn_name = buf[0] ? buf : entry.name;

            std::unique_lock lock(rw_mutex_);
            if (connectors_.contains(scheme)) {
                LOG_WARN("Static connector scheme '{}' already loaded, skipping", scheme);
                continue;
            }

            info->ops    = ops;
            info->name   = std::move(conn_name);
            info->scheme = scheme;
            LOG_INFO("Static connector loaded: '{}' scheme='{}'", info->name, scheme);
            connectors_[scheme] = std::move(info);
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