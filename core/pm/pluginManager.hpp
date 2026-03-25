#pragma once

/// @file core/pm/pluginManager.hpp
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
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../sdk/connector.h"
#include "../sdk/handler.h"

namespace gn {

namespace fs = std::filesystem;

/// @brief Plugin loading, verification, and lifecycle management.
///
/// Thread-safe: all methods use an internal shared_mutex.
/// Heavy implementation details (StringHash, internal maps) are hidden
/// behind the Pimpl to keep the public header lightweight.
class PluginManager {
public:
    explicit PluginManager(host_api_t* api, fs::path plugins_base_dir = {});
    ~PluginManager();

    PluginManager(const PluginManager&)            = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    // ── Loading ───────────────────────────────────────────────────────────────

    /// @brief Load a single plugin from a shared library path.
    /// @param path  Path to the .so/.dylib/.dll file.
    /// @return void on success, error string on failure.
    std::expected<void, std::string> load_plugin   (const fs::path& path);

    /// @brief Recursively scan plugins_base_dir and load all valid plugins.
    void                             load_all_plugins();

    /// @brief Load plugins from the static plugin registry (compiled-in).
    void                             load_static_plugins();

    /// @brief Unload all plugins (shutdown + dlclose).
    void                             unload_all();

    /// @brief Unload a specific handler by name.
    bool unload_handler   (std::string_view name);

    /// @brief Unload a specific connector by URI scheme.
    bool unload_connector (std::string_view scheme);

    /// @brief Enable a disabled handler.
    bool enable_handler   (std::string_view name);

    /// @brief Disable a handler (stops dispatch, keeps loaded).
    bool disable_handler  (std::string_view name);

    // ── Queries ───────────────────────────────────────────────────────────────

    /// @brief Find a handler by name.
    /// @return handler_t* if found and enabled, nullopt otherwise.
    [[nodiscard]] std::optional<handler_t*>       find_handler_by_name    (std::string_view name)   const;

    /// @brief Find a connector by URI scheme.
    /// @return connector_ops_t* if found and enabled, nullopt otherwise.
    [[nodiscard]] std::optional<connector_ops_t*> find_connector_by_scheme(std::string_view scheme) const;

    /// @brief Get all active (enabled) handler descriptors.
    [[nodiscard]] std::vector<handler_t*>       get_active_handlers()        const;

    /// @brief Get all active (enabled) connector vtables.
    [[nodiscard]] std::vector<connector_ops_t*> get_active_connectors()      const;

    /// @brief Get names of all enabled handlers.
    [[nodiscard]] std::vector<std::string>      get_enabled_handler_names()  const;

    /// @brief Count of enabled handlers.
    [[nodiscard]] size_t get_enabled_handler_count()   const;

    /// @brief Count of enabled connectors.
    [[nodiscard]] size_t get_enabled_connector_count() const;

    /// @brief Log all loaded plugins.
    void list_plugins() const;

    // ── Integrity ─────────────────────────────────────────────────────────────

    /// @brief Calculate SHA-256 hash of a file.
    [[nodiscard]] static std::string               calculate_sha256(const fs::path& path);

    /// @brief Verify a plugin's JSON manifest integrity hash.
    [[nodiscard]] std::expected<void, std::string> verify_metadata(const fs::path& so_path) const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace gn
