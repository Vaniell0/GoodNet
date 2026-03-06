#pragma once

/// @file core/pluginManager.hpp
/// @brief Plugin loading, verification, lifecycle management.
///
/// Plugins are .so/.dylib/.dll files exporting either:
///   handler_init(host_api_t*, handler_t**)     — message handlers
///   connector_init(host_api_t*, connector_ops_t**) — transport connectors
///
/// Each plugin ships a companion <plugin>.so.json manifest with SHA-256 integrity hash.
/// Plugins are isolated via RTLD_LOCAL; Logger is injected through api->internal_logger.

#include <string_view>
#include <unordered_map>
#include <shared_mutex>
#include <expected>
#include <vector>
#include <optional>
#include "../sdk/connector.h"
#include "../sdk/handler.h"
#include "../sdk/plugin.h"
#include "dynlib.hpp"

namespace gn {

namespace fs = std::filesystem;

class PluginManager {
public:
    struct HandlerInfo {
        DynLib      lib;
        handler_t*  handler = nullptr;
        fs::path    path;
        std::string name;
        bool        enabled = true;
        host_api_t  api_c;

        HandlerInfo()                              = default;
        ~HandlerInfo();
        HandlerInfo(const HandlerInfo&)            = delete;
        HandlerInfo& operator=(const HandlerInfo&) = delete;
        HandlerInfo(HandlerInfo&&) noexcept        = default;
    };

    struct ConnectorInfo {
        DynLib           lib;
        connector_ops_t* ops     = nullptr;
        fs::path         path;
        std::string      name;
        std::string      scheme;
        bool             enabled = true;
        host_api_t       api_c;

        ConnectorInfo()                              = default;
        ~ConnectorInfo();
        ConnectorInfo(const ConnectorInfo&)            = delete;
        ConnectorInfo& operator=(const ConnectorInfo&) = delete;
        ConnectorInfo(ConnectorInfo&&) noexcept        = default;
    };

private:
    host_api_t* host_api_;
    fs::path    plugins_base_dir_;

    mutable std::shared_mutex rw_mutex_;

    struct StringHash {
        using is_transparent = void;
        size_t operator()(std::string_view sv) const noexcept {
            return std::hash<std::string_view>{}(sv);
        }
    };

    std::unordered_map<std::string, std::unique_ptr<HandlerInfo>,
                       StringHash, std::equal_to<>> handlers_;
    std::unordered_map<std::string, std::unique_ptr<ConnectorInfo>,
                       StringHash, std::equal_to<>> connectors_;

public:
    explicit PluginManager(host_api_t* api, fs::path plugins_base_dir = "");
    ~PluginManager();

    std::expected<void, std::string> load_plugin(const fs::path& path);
    void load_all_plugins();

    std::optional<handler_t*>       find_handler_by_name    (std::string_view name)   const;
    std::optional<connector_ops_t*> find_connector_by_scheme(std::string_view scheme) const;

    size_t get_enabled_handler_count()   const;
    size_t get_enabled_connector_count() const;
    void   list_plugins()                const;

    bool unload_handler (std::string_view name);
    bool enable_handler (std::string_view name);
    bool disable_handler(std::string_view name);
    void unload_all();

    std::vector<handler_t*>       get_active_handlers()   const;
    std::vector<connector_ops_t*> get_active_connectors() const;

    std::expected<void, std::string> verify_metadata(const fs::path& so_path) const;
    static std::string               calculate_sha256(const fs::path& path);
};

} // namespace gn
