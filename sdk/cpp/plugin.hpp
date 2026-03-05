#pragma once

#include "handler.hpp"
#include "connector.hpp"

// ─── HANDLER_PLUGIN ───────────────────────────────────────────────────────────
//
// Использование:
//
//   class MyHandler : public gn::IHandler { ... };
//   HANDLER_PLUGIN(MyHandler)      // в конце .cpp файла
//
// Генерирует:
//   extern "C" GN_EXPORT int handler_init(host_api_t*, handler_t**)
//
// GN_EXPORT = __attribute__((visibility("default"))) на Linux/macOS.
// Без него dlsym("handler_init") вернёт null при -fvisibility=hidden.
//
// Плагин проверяет api->plugin_type == PLUGIN_TYPE_HANDLER чтобы
// не инициализировать хендлер если его загружают как коннектор (ошибка конфига).

#define HANDLER_PLUGIN(ClassName)                                            \
extern "C" GN_EXPORT int handler_init(host_api_t* api, handler_t** out) {   \
    static ClassName instance;                                               \
    if (!api || api->plugin_type != PLUGIN_TYPE_HANDLER) return -1;         \
    instance.init(api);                                                      \
    *out = instance.to_c_handler();                                          \
    return 0;                                                                \
}

// ─── CONNECTOR_PLUGIN ─────────────────────────────────────────────────────────
//
// Использование:
//
//   class MyConnector : public gn::IConnector { ... };
//   CONNECTOR_PLUGIN(MyConnector)  // в конце .cpp файла

#define CONNECTOR_PLUGIN(ClassName)                                              \
extern "C" GN_EXPORT int connector_init(host_api_t* api, connector_ops_t** out) { \
    static ClassName instance;                                                   \
    if (!api || api->plugin_type != PLUGIN_TYPE_CONNECTOR) return -1;           \
    instance.init(api);                                                          \
    *out = instance.to_c_ops();                                                  \
    return 0;                                                                    \
}
