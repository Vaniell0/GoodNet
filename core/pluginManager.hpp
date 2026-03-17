#pragma once

/// @file core/pluginManager.hpp
/// @brief Plugin loading, verification, and lifecycle management.
///
/// Each .so/.dylib/.dll exports one of:
///   - handler_init(host_api_t*, handler_t**)
///   - connector_init(host_api_t*, connector_ops_t**)
///
/// A companion <name>.so.json manifest carries the SHA-256 integrity hash.
/// Plugins are isolated via RTLD_LOCAL; logger is injected through api->internal_logger.

#include <expected>
#include <filesystem>
#include <optional>
#include <shared_mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "types/plugin.hpp"

namespace gn {


namespace fs = std::filesystem;

class PluginManager {
public:
    explicit PluginManager(host_api_t* api, fs::path plugins_base_dir = {});
    ~PluginManager();

    // ── Loading ───────────────────────────────────────────────────────────────

    std::expected<void, std::string> load_plugin   (const fs::path& path);
    void                             load_all_plugins();
    void                             load_static_plugins();
    void                             unload_all();

    bool unload_handler   (std::string_view name);
    bool unload_connector (std::string_view scheme);
    bool enable_handler   (std::string_view name);
    bool disable_handler  (std::string_view name);

    // ── Queries ───────────────────────────────────────────────────────────────

    [[nodiscard]] std::optional<handler_t*>       find_handler_by_name    (std::string_view name)   const;
    [[nodiscard]] std::optional<connector_ops_t*> find_connector_by_scheme(std::string_view scheme) const;

    [[nodiscard]] std::vector<handler_t*>       get_active_handlers()        const;
    [[nodiscard]] std::vector<connector_ops_t*> get_active_connectors()      const;
    [[nodiscard]] std::vector<std::string>      get_enabled_handler_names()  const;

    [[nodiscard]] size_t get_enabled_handler_count()   const;
    [[nodiscard]] size_t get_enabled_connector_count() const;

    void list_plugins() const;

    // ── Integrity ─────────────────────────────────────────────────────────────

    [[nodiscard]] static std::string               calculate_sha256(const fs::path& path);
    [[nodiscard]] std::expected<void, std::string> verify_metadata(const fs::path& so_path) const;

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
};

} // namespace gn