#pragma once

/// @file include/config.hpp
/// @brief Typed hierarchical configuration with JSON persistence.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

// ── Config ────────────────────────────────────────────────────────────────────

/// @brief Typed hierarchical configuration with JSON persistence.
///
/// All settings are typed struct fields with sensible defaults.
/// Direct member access: `cfg.logging.level`, `cfg.compression.enabled`, etc.
class Config {
public:
    // ── Section structs ─────────────────────────────────────────────────────

    /// @brief Core network / runtime settings.
    struct Core {
        std::string listen_address = "0.0.0.0";
        int         listen_port    = 25565;
        int         io_threads     = 0;       ///< 0 = auto (hardware concurrency).
        int         max_connections = 1000;
    };

    /// @brief Logging configuration.
    struct Log {
        std::string level     = "info";
        std::string file;
        int         max_size  = 10'485'760;  ///< Max log file size in bytes (10 MB).
        int         max_files = 5;           ///< Max rotated log files.
    };

    /// @brief Security / auth timeouts.
    struct Security {
        int key_exchange_timeout = 30;    ///< Seconds.
        int max_auth_attempts    = 3;
        int session_timeout      = 3600;  ///< Seconds.
    };

    /// @brief Zstd compression settings for encrypted payloads.
    struct Compression {
        bool enabled   = true;
        int  threshold = 512;  ///< Min payload bytes to trigger compression.
        int  level     = 1;    ///< Zstd compression level.
    };

    /// @brief Plugin loading configuration.
    struct Plugins {
        std::string base_dir;
        bool        auto_load     = true;
        int         scan_interval = 300;   ///< Seconds between directory scans.
        std::string extra_dirs;            ///< Semicolon-separated extra plugin dirs.
    };

    /// @brief Identity key path settings (raw strings, expand_home at use-site).
    struct Identity {
        std::string dir              = "~/.goodnet";
        std::string ssh_key_path;
        bool        use_machine_id   = true;
        bool        skip_ssh_fallback = false;  ///< Не пробовать ~/.ssh/id_ed25519
    };

    /// @brief ICE connector settings.
    struct Ice {
        /// STUN servers as CSV: "host:port,host:port,..."
        std::string stun_servers = "stun.l.google.com:19302,stun1.l.google.com:19302,stun2.l.google.com:19302";
        int session_timeout      = 10;   ///< Seconds.
        int keepalive_interval   = 20;   ///< Seconds.
        int consent_max_failures = 3;
    };

    // ── Sections (direct access) ─────────────────────────────────────────────

    Core        core;
    Log         logging;
    Security    security;
    Compression compression;
    Plugins     plugins;
    Identity    identity;
    Ice         ice;

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
