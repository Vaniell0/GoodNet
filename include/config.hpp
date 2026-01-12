#pragma once

#include <filesystem>
#include <unordered_map>
#include <optional>
#include <variant>
#include <string>
#include "logger.hpp"

namespace fs = std::filesystem;

class Config {
public:
    struct Core {
        static const std::string LISTEN_ADDRESS;
        static const uint LISTEN_PORT;
        static const short IO_THREADS;
        static const size_t MAX_CONNECTIONS;
    };
    
    struct Logging {
        static const std::string LEVEL;
        static const std::string FILE;
        static const int MAX_SIZE;
        static const int MAX_FILES;
    };
    
    struct Plugins {
        static const fs::path BASE_DIR;
        static const bool AUTO_LOAD;
        static const int SCAN_INTERVAL;
    };
    
    struct Security {
        static const int KEY_EXCHANGE_TIMEOUT;
        static const int MAX_AUTH_ATTEMPTS;
        static const int SESSION_TIMEOUT;
    };

    using Value = std::variant<int, bool, double, std::string, fs::path>;
    
    Config();
    ~Config();
    
    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;
    
    Config(Config&&) = default;
    Config& operator=(Config&&) = default;

    bool load_from_file(const fs::path& config_file);
    bool load_from_string(const std::string& config_str) {
        return parse_json(config_str);
    }

    bool save_to_file(const fs::path& config_file) const;
    std::string save_to_string() const {
        return to_json();
    }

    template<typename T>
    void set(const std::string& key, const T& value) {
        Value var_value = value;
        values_[key] = var_value;
        
        // Логируем установку значения
        std::visit([&](const auto& val) {
            using U = std::decay_t<decltype(val)>;
            if constexpr (std::is_same_v<U, fs::path>) {
                LOG_DEBUG("Config set: {} = {}", key, val.string());
            } else if constexpr (std::is_same_v<U, std::string>) {
                LOG_DEBUG("Config set: {} = {}", key, val);
            } else if constexpr (std::is_same_v<U, int> || std::is_same_v<U, bool> || 
                               std::is_same_v<U, double>) {
                LOG_DEBUG("Config set: {} = {}", key, val);
            }
        }, var_value);
    }

    void set(const std::string& key, const char* value) {
        set<std::string>(key, std::string(value));
    }
    
    template<typename T>
    std::optional<T> get(const std::string& key) const {
        auto it = values_.find(key);
        if (it != values_.end()) {
            try {
                if constexpr (std::is_same_v<T, fs::path>) {
                    auto str_val = std::get<std::string>(it->second);
                    return fs::path(str_val);
                } else {
                    return std::get<T>(it->second);
                }
            } catch (const std::bad_variant_access& e) {
                LOG_ERROR("Config type mismatch for key '{}': {}", key, e.what());
            }
        }
        return std::nullopt;
    }
    
    template<typename T>
    T get_or(const std::string& key, const T& default_value) const {
        auto val = get<T>(key);
        return val.has_value() ? val.value() : default_value;
    }
    
    bool has(const std::string& key) const;
    void remove(const std::string& key);
    
    const std::unordered_map<std::string, Value>& all() const { return values_; }

private:
    std::unordered_map<std::string, Value> values_;
    
    void setup_defaults();
    bool parse_json(const std::string& json_str);
    std::string to_json() const;
};
