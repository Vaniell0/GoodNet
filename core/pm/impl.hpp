#pragma once
/// @file core/pm/impl.hpp
/// @brief PluginManager::Impl — private implementation details.

#include "pluginManager.hpp"
#include "types/plugin.hpp"

#include <shared_mutex>
#include <unordered_map>

namespace gn {

/// @brief Transparent hash for heterogeneous string lookups.
struct StringHash {
    using is_transparent = void;
    size_t operator()(std::string_view sv) const noexcept {
        return std::hash<std::string_view>{}(sv);
    }
};

struct PluginManager::Impl {
    host_api_t*        host_api;
    fs::path           plugins_base_dir;

    mutable std::shared_mutex rw_mutex;

    std::unordered_map<std::string, std::unique_ptr<HandlerInfo>,
                       StringHash, std::equal_to<>> handlers;
    std::unordered_map<std::string, std::unique_ptr<ConnectorInfo>,
                       StringHash, std::equal_to<>> connectors;

    explicit Impl(host_api_t* api, fs::path base_dir)
        : host_api(api), plugins_base_dir(std::move(base_dir)) {}
};

} // namespace gn
