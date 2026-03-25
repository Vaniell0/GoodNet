#pragma once
/// @file include/static_registry.hpp
/// @brief Compile-time plugin registry for static linking.
///
/// When GOODNET_STATIC_PLUGINS is defined, HANDLER_PLUGIN / CONNECTOR_PLUGIN
/// macros register into this global table instead of exporting extern "C"
/// symbols. PluginManager::load_static_plugins() iterates the registry
/// and initializes each entry exactly like a dynamically-loaded plugin.

#include "../sdk/handler.h"
#include "../sdk/connector.h"
#include <vector>

namespace gn {

struct StaticPluginEntry {
    const char*      name;
    handler_init_t   handler_init   = nullptr;
    connector_init_t connector_init = nullptr;
};

inline std::vector<StaticPluginEntry>& static_plugin_registry() {
    static std::vector<StaticPluginEntry> registry;
    return registry;
}

} // namespace gn
