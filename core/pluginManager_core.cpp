#include "pluginManager.hpp"
#include "logger.hpp"

#include "../sdk/handler.h"
#include "../sdk/connector.h"
#include "../sdk/plugin.h"
#include <dlfcn.h>
#include <fmt/format.h>

namespace gn {

// ─── Деструкторы ─────────────────────────────────────────────────────────────
// Реализованы здесь: компилятор видит полные определения handler_t / connector_ops_t.

PluginManager::HandlerInfo::~HandlerInfo() {
    if (!dl_handle) return;
    if (handler && handler->shutdown)
        handler->shutdown(handler->user_data);
    dlclose(dl_handle);
}

PluginManager::ConnectorInfo::~ConnectorInfo() {
    if (!dl_handle) return;
    if (ops && ops->shutdown)
        ops->shutdown(ops->connector_ctx);
    dlclose(dl_handle);
}

// ─── Конструктор / деструктор ─────────────────────────────────────────────────

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

    std::unique_lock lock(rw_mutex_);

    // RTLD_GLOBAL: плагин видит символы libgoodnet_core.so / исполняемого файла.
    // Без этого первый LOG_* в плагине дереференсирует нулевой Logger::logger_ → SIGSEGV.
    // RTLD_NOW:    неразрешённые символы — ошибка сразу, не при первом вызове.
    ::dlerror();
    void* handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const char* e = ::dlerror();
        return std::unexpected(fmt::format("dlopen failed: {}",
                                            e ? e : "(unknown)"));
    }

    // ── Обработчик (Handler) ──────────────────────────────────────────────────
    //
    // Сигнатура точки входа (из sdk/cpp/plugin.hpp HANDLER_PLUGIN макроса):
    //   extern "C" int handler_init(host_api_t* api, handler_t** handler)
    //
    // Возвращает 0 при успехе, -1 при ошибке.
    // api->plugin_type должен быть PLUGIN_TYPE_HANDLER.

    using handler_init_fn = handler_t*(*)(host_api_t*);
    ::dlerror();
    auto handler_init_sym = reinterpret_cast<handler_init_fn>(
        reinterpret_cast<uintptr_t>(::dlsym(handle, "handler_init")));

    if (handler_init_sym && !::dlerror()) {
        host_api_t handler_api = *host_api_;
        handler_api.plugin_type = PLUGIN_TYPE_HANDLER;
        handler_api.internal_logger = host_api_->internal_logger;

        handler_t* h = handler_init_sym(&handler_api);
        
        if (!h) {
            ::dlclose(handle);
            return std::unexpected("handler_init() returned nullptr");
        }

        auto info       = std::make_unique<HandlerInfo>();
        info->dl_handle = handle;
        info->handler   = h;
        info->path      = path;
        info->name      = path.stem().string();

        const std::string name = info->name;
        handlers_[name] = std::move(info);
        LOG_INFO("Loaded Handler: '{}' [{}]", name, path.filename().string());
        return {};
    }

    // ── Коннектор (Connector) ─────────────────────────────────────────────────
    //
    // Сигнатура (из sdk/cpp/plugin.hpp CONNECTOR_PLUGIN макроса):
    //   extern "C" int connector_init(host_api_t* api, connector_ops_t** ops)

    using connector_init_fn = connector_ops_t*(*)(host_api_t*);
    ::dlerror();
    auto connector_init_sym = reinterpret_cast<connector_init_fn>(
        reinterpret_cast<uintptr_t>(::dlsym(handle, "connector_init")));

    if (connector_init_sym && !::dlerror()) {
        host_api_t connector_api = *host_api_;
        connector_api.plugin_type = PLUGIN_TYPE_CONNECTOR;
        connector_api.internal_logger = host_api_->internal_logger;

        connector_ops_t* ops = connector_init_sym(&connector_api);
        
        if (!ops) {
            ::dlclose(handle);
            return std::unexpected("connector_init() returned nullptr");
        }

        // Извлекаем name и scheme через геттеры
        char buf[256];
        std::string conn_name, conn_scheme;

        if (ops->get_name) {
            buf[0] = '\0';
            ops->get_name(ops->connector_ctx, buf, sizeof(buf));
            conn_name = buf;
        }
        if (conn_name.empty()) conn_name = path.stem().string();

        if (ops->get_scheme) {
            buf[0] = '\0';
            ops->get_scheme(ops->connector_ctx, buf, sizeof(buf));
            conn_scheme = buf;
        }
        if (conn_scheme.empty()) {
            ::dlclose(handle);
            return std::unexpected(fmt::format(
                "Connector '{}': get_scheme() returned empty", conn_name));
        }

        auto info        = std::make_unique<ConnectorInfo>();
        info->dl_handle  = handle;
        info->ops        = ops;
        info->path       = path;
        info->name       = std::move(conn_name);
        info->scheme     = conn_scheme;

        LOG_INFO("Loaded Connector: '{}' scheme='{}' [{}]",
         info->name, conn_scheme, path.filename());
        connectors_[conn_scheme] = std::move(info);
        return {};
    }

    ::dlclose(handle);
    return std::unexpected(
        "Plugin exports neither 'handler_init' nor 'connector_init'");
}

// ─── unload_all ───────────────────────────────────────────────────────────────

void PluginManager::unload_all() {
    std::unique_lock lock(rw_mutex_);
    LOG_INFO("Unloading all plugins ({} handlers, {} connectors)",
             handlers_.size(), connectors_.size());
    handlers_.clear();
    connectors_.clear();
}

} // namespace gn
