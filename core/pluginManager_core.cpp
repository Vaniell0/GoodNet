#include "pluginManager.hpp"
#include "logger.hpp"

namespace gn {

// ─── Деструкторы ─────────────────────────────────────────────────────────────

PluginManager::HandlerInfo::~HandlerInfo() {
    if (handler && handler->shutdown)
        handler->shutdown(handler->user_data);
    // lib.close() вызовется автоматически деструктором DynLib
}

PluginManager::ConnectorInfo::~ConnectorInfo() {
    if (ops && ops->shutdown)
        ops->shutdown(ops->connector_ctx);
}

// ─── Конструктор ─────────────────────────────────────────────────────────────

PluginManager::PluginManager(host_api_t* api, fs::path plugins_base_dir)
    : host_api_(api), plugins_base_dir_(std::move(plugins_base_dir))
{
    LOG_INFO("PluginManager initialized. Base dir: {}", plugins_base_dir_.string());
}

PluginManager::~PluginManager() {
    unload_all();
}

// ─── load_plugin ─────────────────────────────────────────────────────────────

std::expected<void, std::string> PluginManager::load_plugin(const fs::path& path) {
    if (!fs::exists(path))
        return std::unexpected(fmt::format("Plugin not found: {}", path.string()));

    if (auto v = verify_metadata(path); !v) {
        LOG_ERROR("Integrity check failed for '{}': {}", path.filename().string(), v.error());
        return std::unexpected(v.error());
    }

    // 1. Открываем библиотеку (RAII)
    auto lib_res = DynLib::open(path);
    if (!lib_res)
        return std::unexpected(lib_res.error());

    DynLib lib = std::move(*lib_res);
    std::unique_lock lock(rw_mutex_);

    // 2. Пробуем загрузить как Handler
    using h_init_t = handler_t*(*)(host_api_t*);
    auto h_sym = lib.symbol<h_init_t>("handler_init");

    if (h_sym) {
        host_api_t api = *host_api_;
        api.plugin_type = PLUGIN_TYPE_HANDLER;
        
        handler_t* h = (*h_sym)(&api);
        if (!h) return std::unexpected("handler_init() returned nullptr");

        auto info    = std::make_unique<HandlerInfo>();
        info->lib     = std::move(lib);
        info->handler = h;
        info->path    = path;
        info->name = (h->name && strlen(h->name) > 0) ? h->name : path.stem().string();

        handlers_[info->name] = std::move(info);
        LOG_INFO("Loaded Handler: '{}'", path.stem().string());
        return {};
    }

    // 3. Пробуем загрузить как Connector
    using c_init_t = connector_ops_t*(*)(host_api_t*);
    auto c_sym = lib.symbol<c_init_t>("connector_init");

    if (c_sym) {
        host_api_t api = *host_api_;
        api.plugin_type = PLUGIN_TYPE_CONNECTOR;

        connector_ops_t* ops = (*c_sym)(&api);
        if (!ops) return std::unexpected("connector_init() returned nullptr");

        char buf[256] = {0};
        std::string c_name, c_scheme;

        if (ops->get_name) {
            ops->get_name(ops->connector_ctx, buf, sizeof(buf));
            c_name = buf;
        }
        if (c_name.empty()) { c_name = path.stem().string(); }

        if (ops->get_scheme) {
            buf[0] = '\0';
            ops->get_scheme(ops->connector_ctx, buf, sizeof(buf));
            c_scheme = buf;
        }

        if (c_scheme.empty())
            return std::unexpected(fmt::format("Connector '{}' has no scheme", c_name));

        auto info    = std::make_unique<ConnectorInfo>();
        info->lib     = std::move(lib);
        info->ops     = ops;
        info->path    = path;
        info->name    = std::move(c_name);
        info->scheme  = c_scheme;

        LOG_INFO("Loaded Connector: '{}' [{}]", info->name, info->scheme);
        connectors_[info->scheme] = std::move(info);
        return {};
    }

    return std::unexpected("Plugin has no known entry points");
}

void PluginManager::unload_all() {
    std::unique_lock lock(rw_mutex_);
    handlers_.clear();
    connectors_.clear();
}

} // namespace gn
