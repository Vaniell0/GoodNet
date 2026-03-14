#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>

namespace fs = std::filesystem;

// ── Config ────────────────────────────────────────────────────────────────────

/// Flat key-value config store with JSON persistence.
/// Keys use dot notation: "core.listen_port", "logging.level", etc.
class Config {
public:
    // ── Default keys ──────────────────────────────────────────────────────────

    struct Core {
        static const std::string LISTEN_ADDRESS;
        static const unsigned    LISTEN_PORT;
        static const short       IO_THREADS;
        static const size_t      MAX_CONNECTIONS;
    };
    struct Logging {
        static const std::string LEVEL;
        static const std::string FILE;
        static const int         MAX_SIZE;
        static const int         MAX_FILES;
    };
    struct Plugins {
        static const fs::path BASE_DIR;
        static const bool     AUTO_LOAD;
        static const int      SCAN_INTERVAL;
    };
    struct Security {
        static const int KEY_EXCHANGE_TIMEOUT;
        static const int MAX_AUTH_ATTEMPTS;
        static const int SESSION_TIMEOUT;
    };

    // ── Value type ────────────────────────────────────────────────────────────

    using Value = std::variant<int, bool, double, std::string, fs::path>;

    // ── Construction ──────────────────────────────────────────────────────────

    explicit Config(bool defaults_only = false);

    Config(const Config&)            = delete;
    Config& operator=(const Config&) = delete;
    Config(Config&&)                 = default;
    Config& operator=(Config&&)      = default;

    // ── Persistence ───────────────────────────────────────────────────────────

    bool        load_from_file  (const fs::path&    path);
    bool        load_from_string(const std::string& json);
    bool        save_to_file    (const fs::path&    path) const;
    std::string save_to_string  ()                         const;

    /// Reload from the last successfully loaded file path.
    bool reload();

    // ── Mutation ──────────────────────────────────────────────────────────────

    template<typename T>
    void set(const std::string& key, const T& value) {
        values_[key] = Value{value};
        notify(key);
    }

    void set(const std::string& key, const char* value) {
        set<std::string>(key, std::string(value));
    }

    void remove(const std::string& key);

    // ── Access ────────────────────────────────────────────────────────────────

    template<typename T>
    [[nodiscard]] std::optional<T> get(const std::string& key) const;

    template<typename T>
    [[nodiscard]] T get_or(const std::string& key, const T& fallback) const {
        auto v = get<T>(key);
        return v ? *v : fallback;
    }

    [[nodiscard]] bool has(const std::string& key) const;

    /// Return all keys whose prefix matches a section, e.g. "core".
    [[nodiscard]] std::vector<std::string> keys_in(std::string_view section) const;

    [[nodiscard]] const std::unordered_map<std::string, Value>& all() const { return values_; }

    // ── Change notifications ──────────────────────────────────────────────────

    using WatchCallback = std::function<void(const std::string& key, const Value&)>;

    /// Register a callback invoked whenever `key` changes.
    /// Returns a token ID; call unwatch(id) to deregister.
    uint64_t watch  (const std::string& key, WatchCallback cb);
    void     unwatch(uint64_t token_id);

private:
    void        setup_defaults();
    bool        parse_json(const std::string& json);
    std::string to_json()  const;
    void        notify(const std::string& key);

    std::unordered_map<std::string, Value> values_;
    fs::path last_file_;

    struct Watcher { std::string key; WatchCallback cb; };
    std::unordered_map<uint64_t, Watcher> watchers_;
    uint64_t next_watch_id_ = 1;
};

// ── Explicit instantiation declarations ──────────────────────────────────────

extern template std::optional<int>         Config::get<int>        (const std::string&) const;
extern template std::optional<bool>        Config::get<bool>       (const std::string&) const;
extern template std::optional<double>      Config::get<double>     (const std::string&) const;
extern template std::optional<std::string> Config::get<std::string>(const std::string&) const;
extern template std::optional<fs::path>    Config::get<fs::path>   (const std::string&) const;
