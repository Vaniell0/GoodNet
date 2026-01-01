#include "config.hpp"
#include "logger.hpp"

#include <string>
#include <sstream>
#include <fstream>
#include <nlohmann/json.hpp>

const short Config::Defaults::IO_THREADS = 4;
const std::string Config::Defaults::LISTEN_ADDRESS = "0.0.0.0";
const uint Config::Defaults::LISTEN_PORT = 25565;
const std::string Config::Defaults::LOG_LEVEL = "INFO";
const fs::path Config::Defaults::DEFAULT_PLUGINS_DIR = fs::current_path() / "plugins";
const bool Config::Defaults::AUTO_LOAD_PLUGINS = true;
const size_t Config::Defaults::MAX_CONNECTIONS = 1000;

LOGGER("CONFIG");
inline fs::path root = fs::current_path();

Logger& Config::logger_() const { return logger; }

Config::Config() {
    setup_defaults();
    load_from_file(root / "config.json");
}

void Config::setup_defaults() {
    // Сетевая конфигурация
    set("core.listen_address", Defaults::LISTEN_ADDRESS);
    set("core.listen_port", static_cast<int>(Defaults::LISTEN_PORT));
    set("core.io_threads", static_cast<int>(Defaults::IO_THREADS));
    set("core.max_connections", static_cast<int>(Defaults::MAX_CONNECTIONS));
    
    // Логирование
    set("logging.level", Defaults::LOG_LEVEL);
    set("logging.file", std::string("logs/goodnet.log"));
    set("logging.max_size", static_cast<int>(10 * 1024 * 1024)); // 10MB
    set("logging.max_files", static_cast<int>(5));
    
    // Плагины
    set("plugins.base_dir", Defaults::DEFAULT_PLUGINS_DIR);
    set("plugins.auto_load", Defaults::AUTO_LOAD_PLUGINS);
    set("plugins.scan_interval", static_cast<int>(300)); // 5 минут
    
    // Безопасность
    set("security.key_exchange_timeout", static_cast<int>(30)); // секунды
    set("security.max_auth_attempts", static_cast<int>(3));
    set("security.session_timeout", static_cast<int>(3600)); // 1 час
    
    logger.info("Default configuration loaded");
}

bool Config::load_from_file(const fs::path& config_file) {
    if (!fs::exists(config_file)) {
        logger.warning("Config file not found: {}", config_file.string());
        return false;
    }
    
    try {
        std::ifstream file(config_file);
        if (!file.is_open()) {
            logger.error("Failed to open config file: {}", config_file.string());
            return false;
        }
        
        std::stringstream buffer;
        buffer << file.rdbuf();
        
        return parse_json(buffer.str());
        
    } catch (const std::exception& e) {
        logger.error("Error loading config from {}: {}", config_file.string(), e.what());
        return false;
    }
}

bool Config::parse_json(const std::string& json_str) {
    try {
        nlohmann::json j = nlohmann::json::parse(json_str);
        
        // Рекурсивно обходим JSON и загружаем значения
        std::function<void(const std::string&, const nlohmann::json&)> parse_object;
        parse_object = [&](const std::string& prefix, const nlohmann::json& obj) {
            for (auto it = obj.begin(); it != obj.end(); ++it) {
                std::string key = prefix.empty() ? it.key() : prefix + "." + it.key();
                
                if (it.value().is_object()) {
                    parse_object(key, it.value());
                } else if (it.value().is_string()) {
                    std::string val = it.value().get<std::string>();
                    set(key, val);
                } else if (it.value().is_number_integer()) {
                    set(key, it.value().get<int>());
                } else if (it.value().is_number_float()) {
                    set(key, it.value().get<double>());
                } else if (it.value().is_boolean()) {
                    set(key, it.value().get<bool>());
                }
            }
        };
        
        parse_object("", j);
        logger.info("Configuration loaded from JSON");
        return true;
        
    } catch (const nlohmann::json::exception& e) {
        logger.error("JSON parsing error: {}", e.what());
        return false;
    }
}

std::string Config::to_json() const {
    nlohmann::json j;
    
    for (const auto& [key, value] : values_) {
        std::vector<std::string> parts;
        std::stringstream ss(key);
        std::string part;
        
        while (std::getline(ss, part, '.')) {
            parts.push_back(part);
        }
        
        nlohmann::json* current = &j;
        for (size_t i = 0; i < parts.size() - 1; ++i) {
            if (!current->contains(parts[i])) {
                (*current)[parts[i]] = nlohmann::json::object();
            }
            current = &(*current)[parts[i]];
        }
        
        std::visit([&](const auto& val) {
            if constexpr (std::is_same_v<std::decay_t<decltype(val)>, fs::path>) {
                (*current)[parts.back()] = val.string();
            } else {
                (*current)[parts.back()] = val;
            }
        }, value);
    }
    
    return j.dump(2);
}

bool Config::save_to_file(const fs::path& config_file) const {
    try {
        // Создаём директорию если нужно
        fs::create_directories(config_file.parent_path());
        
        std::ofstream file(config_file);
        if (!file.is_open()) {
            logger.error("Failed to open config file for writing: {}", 
                         config_file.string());
            return false;
        }
        
        file << to_json();
        logger.info("Configuration saved to: {}", config_file.string());
        return true;
        
    } catch (const std::exception& e) {
        logger.error("Error saving config to {}: {}", config_file.string(), e.what());
        return false;
    }
}

bool Config::has(const std::string& key) const {
    return values_.find(key) != values_.end();
}

void Config::remove(const std::string& key) {
    auto it = values_.find(key);
    if (it != values_.end()) {
        values_.erase(it);
        logger.debug("Config removed: {}", key);
    }
}

// Внешние определения шаблонных методов
template<typename T>
void Config::set(const std::string& key, const T& value) {
    values_[key] = value;
    // Для fs::path используем .string()
    if constexpr (std::is_same_v<T, fs::path>) {
        logger_().debug("Config set: {} = {}", key, value.string());
    } else {
        logger_().debug("Config set: {} = {}", key, value);
    }
}

template<typename T>
std::optional<T> Config::get(const std::string& key) const {
    auto it = values_.find(key);
    if (it != values_.end()) {
        try {
            return std::get<T>(it->second);
        } catch (const std::bad_variant_access&) {
            logger_().error("Config type mismatch for key: {}", key);
        }
    } return std::nullopt;
}

template<typename T>
T Config::get_or(const std::string& key, const T& default_value) const {
    auto val = get<T>(key);
    return val.has_value() ? val.value() : default_value;
}
