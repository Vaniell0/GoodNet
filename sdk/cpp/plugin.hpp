#pragma once
#include "handler.h"
#include "connector.h"
#include "logger.hpp"
#include <memory>

#if defined(_WIN32)
    #define GN_EXPORT __declspec(dllexport)
#else
    #define GN_EXPORT __attribute__((visibility("default")))
#endif

// Вспомогательная логика синхронизации
inline void sync_plugin_context(host_api_t* api) {
    if (api && api->internal_logger) {
        // Создаем shared_ptr, который НЕ удаляет объект (так как им владеет ядро)
        auto core_logger = std::shared_ptr<spdlog::logger>(
            static_cast<spdlog::logger*>(api->internal_logger),
            [](spdlog::logger*){} 
        );
        Logger::set_external_logger(core_logger);
    }
}

#define HANDLER_PLUGIN(ClassName)                               \
extern "C" GN_EXPORT handler_t* handler_init(host_api_t* api) { \
    if (!api) return nullptr;                                   \
    sync_plugin_context(api);                                   \
    static ClassName instance;                                  \
    instance.init(api);                                         \
    return instance.to_c_handler();                             \
}

#define CONNECTOR_PLUGIN(ClassName)                                     \
extern "C" GN_EXPORT connector_ops_t* connector_init(host_api_t* api) { \
    if (!api) return nullptr;                                           \
    sync_plugin_context(api);                                           \
    static ClassName instance;                                          \
    instance.init(api);                                                 \
    return instance.to_c_ops();                                         \
}
