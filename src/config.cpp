#include "config.hpp"
#include <string>
#include <sstream>
#include <fstream>
#include <nlohmann/json.hpp>
#include <iostream>

// Инициализация статических констант по модулям
const std::string Config::Core::LISTEN_ADDRESS = "0.0.0.0";
const uint Config::Core::LISTEN_PORT = 25565;
const short Config::Core::IO_THREADS = 4;
const size_t Config::Core::MAX_CONNECTIONS = 1000;

const std::string Config::Logging::LEVEL = "info";
const std::string Config::Logging::FILE = "logs/goodnet.log";
const int Config::Logging::MAX_SIZE = 10 * 1024 * 1024;
const int Config::Logging::MAX_FILES = 5;

const fs::path Config::Plugins::BASE_DIR = fs::current_path() / "plugins";
const bool Config::Plugins::AUTO_LOAD = true;
const int Config::Plugins::SCAN_INTERVAL = 300;

const int Config::Security::KEY_EXCHANGE_TIMEOUT = 30;
const int Config::Security::MAX_AUTH_ATTEMPTS = 3;
const int Config::Security::SESSION_TIMEOUT = 3600;

Config::Config() {
    setup_defaults();
    load_from_file(fs::current_path() / "config.json");
}

Config::~Config() {
    LOG_INFO("Config destroyed");
}

void Config::setup_defaults() {
    // Core модуль
    set("core.listen_address", Core::LISTEN_ADDRESS);
    set("core.listen_port", static_cast<int>(Core::LISTEN_PORT));
    set("core.io_threads", static_cast<int>(Core::IO_THREADS));
    set("core.max_connections", static_cast<int>(Core::MAX_CONNECTIONS));
    
    // Logging модуль
    set("logging.level", Logging::LEVEL);
    set("logging.file", Logging::FILE);
    set("logging.max_size", Logging::MAX_SIZE);
    set("logging.max_files", Logging::MAX_FILES);
    
    // Plugins модуль
    set("plugins.base_dir", Plugins::BASE_DIR);
    set("plugins.auto_load", Plugins::AUTO_LOAD);
    set("plugins.scan_interval", Plugins::SCAN_INTERVAL);
    
    // Security модуль
    set("security.key_exchange_timeout", Security::KEY_EXCHANGE_TIMEOUT);
    set("security.max_auth_attempts", Security::MAX_AUTH_ATTEMPTS);
    set("security.session_timeout", Security::SESSION_TIMEOUT);
    
    LOG_INFO("Default configuration loaded");
}

bool Config::load_from_file(const fs::path& config_file) {
    if (!fs::exists(config_file)) {
        LOG_WARN("Config file not found: {}, using defaults", config_file.string());
        return false;
    }
    
    try {
        std::ifstream file(config_file);
        if (!file.is_open()) {
            LOG_ERROR("Failed to open config file: {}", config_file.string());
            return false;
        }
        
        std::stringstream buffer;
        buffer << file.rdbuf();
        
        bool result = parse_json(buffer.str());
        if (result) {
            LOG_INFO("Configuration loaded from: {}", config_file.string());
        }
        return result;
        
    } catch (const std::exception& e) {
        LOG_ERROR("Error loading config from {}: {}", config_file.string(), e.what());
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
        LOG_INFO("Configuration loaded from JSON");
        return true;
        
    } catch (const nlohmann::json::exception& e) {
        LOG_ERROR("JSON parsing error: {}", e.what());
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
            LOG_ERROR("Failed to open config file for writing: {}", 
                     config_file.string());
            return false;
        }
        
        file << to_json();
        LOG_INFO("Configuration saved to: {}", config_file.string());
        return true;
        
    } catch (const std::exception& e) {
        LOG_ERROR("Error saving config to {}: {}", config_file.string(), e.what());
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
        LOG_DEBUG("Config removed: {}", key);
    }
}

// Явное инстанцирование для используемых типов
template void Config::set<int>(const std::string&, const int&);
template void Config::set<bool>(const std::string&, const bool&);
template void Config::set<double>(const std::string&, const double&);
template void Config::set<std::string>(const std::string&, const std::string&);
template void Config::set<fs::path>(const std::string&, const fs::path&);

template std::optional<int> Config::get<int>(const std::string&) const;
template std::optional<bool> Config::get<bool>(const std::string&) const;
template std::optional<double> Config::get<double>(const std::string&) const;
template std::optional<std::string> Config::get<std::string>(const std::string&) const;
template std::optional<fs::path> Config::get<fs::path>(const std::string&) const;

template int Config::get_or<int>(const std::string&, const int&) const;
template bool Config::get_or<bool>(const std::string&, const bool&) const;
template double Config::get_or<double>(const std::string&, const double&) const;
template std::string Config::get_or<std::string>(const std::string&, const std::string&) const;
template fs::path Config::get_or<fs::path>(const std::string&, const fs::path&) const;