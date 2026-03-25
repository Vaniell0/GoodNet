#pragma once

/// @file include/config.hpp
/// @brief Typed hierarchical configuration with JSON persistence.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

// ── Section structs ──────────────────────────────────────────────────────────

/// @brief Core network / runtime settings.
struct CoreConfig {
    std::string listen_address = "0.0.0.0";
    int         listen_port    = 25565;
    int         io_threads     = 0;       ///< 0 = auto (hardware concurrency).
    int         max_connections = 1000;
};

/// @brief Logging configuration.
struct LogConfig {
    std::string level     = "info";
    std::string file;
    int         max_size  = 10'485'760;  ///< Max log file size in bytes (10 MB).
    int         max_files = 5;           ///< Max rotated log files.
};

/// @brief Security / auth timeouts.
struct SecurityConfig {
    int key_exchange_timeout = 30;    ///< Seconds.
    int max_auth_attempts    = 3;
    int session_timeout      = 3600;  ///< Seconds.
};

/// @brief Zstd compression settings for encrypted payloads.
struct CompressionConfig {
    bool enabled   = true;
    int  threshold = 512;  ///< Min payload bytes to trigger compression.
    int  level     = 1;    ///< Zstd compression level.
};

/// @brief Plugin loading configuration.
struct PluginsConfig {
    std::string base_dir;
    bool        auto_load     = true;
    int         scan_interval = 300;   ///< Seconds between directory scans.
    std::string extra_dirs;            ///< Semicolon-separated extra plugin dirs.
};

/// @brief Identity key path settings (raw strings, expand_home at use-site).
struct IdentitySection {
    std::string dir            = "~/.goodnet";
    std::string ssh_key_path;
    bool        use_machine_id = true;
};

// ── Config ────────────────────────────────────────────────────────────────────

/// @brief Typed hierarchical configuration with JSON persistence.
///
/// All settings are typed struct fields with sensible defaults.
/// Direct member access: `cfg.logging.level`, `cfg.compression.enabled`, etc.
class Config {
public:
    // ── Sections (direct access) ─────────────────────────────────────────────

    CoreConfig        core;
    LogConfig         logging;
    SecurityConfig    security;
    CompressionConfig compression;
    PluginsConfig     plugins;
    IdentitySection   identity;

    // ── Construction ─────────────────────────────────────────────────────────

    /// @brief Construct config with defaults.
    /// @param defaults_only  If false, also try loading config.json from CWD.
    explicit Config(bool defaults_only = false);

    // ── Persistence ──────────────────────────────────────────────────────────

    bool        load_from_file  (const fs::path& path);
    bool        load_from_string(const std::string& json);
    bool        save_to_file    (const fs::path& path) const;
    std::string save_to_string  () const;

    /// @brief Reload from the last successfully loaded file path.
    bool reload();

    // ── Flat key access (CAPI / plugin backward-compat) ──────────────────────

    /// @brief Read a known config value as string by dotted key.
    /// @return nullopt if the key is not recognized.
    [[nodiscard]] std::optional<std::string> get_raw(std::string_view key) const;

private:
    fs::path last_file_;

    bool        parse_json(const std::string& json);
    std::string to_json()  const;
};
