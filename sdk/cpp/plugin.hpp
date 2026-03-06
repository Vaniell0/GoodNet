#pragma once
#include "handler.hpp"
#include "connector.hpp"

/// @defgroup plugin_macros Plugin Entry Point Macros
/// @brief Generates the exported `handler_init` / `connector_init` C functions.
///
/// ## Usage
/// ```cpp
/// class MyHandler : public gn::IHandler { ... };
/// HANDLER_PLUGIN(MyHandler)      // at end of .cpp
///
/// class MyConnector : public gn::IConnector { ... };
/// CONNECTOR_PLUGIN(MyConnector)  // at end of .cpp
/// ```
///
/// ## Why `static` instance
/// The plugin is loaded via `dlopen(RTLD_LOCAL)` — one .so, one instance.
/// `static` guarantees a single object with deterministic lifetime.
///
/// ## plugin_type guard
/// Verifies the caller is loading the plugin in the correct role.
/// Prevents misconfiguration where a handler .so is loaded as a connector.
/// @{

/// @brief Generate `handler_init` for a class derived from gn::IHandler.
/// @param ClassName  Concrete handler class (must be default-constructible)
#define HANDLER_PLUGIN(ClassName)                                             \
extern "C" GN_EXPORT int handler_init(host_api_t* api, handler_t** out) {    \
    static ClassName instance;                                                \
    if (!api || api->plugin_type != PLUGIN_TYPE_HANDLER) return -1;          \
    instance.init(api);                                                       \
    *out = instance.to_c_handler();                                           \
    return 0;                                                                 \
}

/// @brief Generate `connector_init` for a class derived from gn::IConnector.
/// @param ClassName  Concrete connector class (must be default-constructible)
#define CONNECTOR_PLUGIN(ClassName)                                               \
extern "C" GN_EXPORT int connector_init(host_api_t* api, connector_ops_t** out) { \
    static ClassName instance;                                                    \
    if (!api || api->plugin_type != PLUGIN_TYPE_CONNECTOR) return -1;            \
    instance.init(api);                                                           \
    *out = instance.to_c_ops();                                                   \
    return 0;                                                                     \
}

/// @}  // defgroup plugin_macros
