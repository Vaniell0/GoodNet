#pragma once
#include <filesystem>
#include <unordered_map>
#include <optional>
#include <variant>

namespace fs = std::filesystem;
class Logger;

class Config {
    struct Defaults {
        static const std::string LISTEN_ADDRESS;
        static const uint LISTEN_PORT;
        static const short IO_THREADS;
        static const std::string LOG_LEVEL;
        static const fs::path DEFAULT_PLUGINS_DIR;
        static const bool AUTO_LOAD_PLUGINS;
        static const size_t MAX_CONNECTIONS;
    };

public:
    using Value = std::variant<int, bool, double, std::string, fs::path>;
    
    Config();
    ~Config() = default;

    // Загрузка конфигурации из файла
    bool load_from_file(const fs::path& config_file);
    bool load_from_string(const std::string& config_str);

    // Сохранение конфигурации
    bool save_to_file(const fs::path& config_file) const;
    std::string save_to_string() const;

    // Работа с настройками
    template<typename T>
    void set(const std::string& key, const T& value);
    
    template<typename T>
    std::optional<T> get(const std::string& key) const;
    
    template<typename T>
    T get_or(const std::string& key, const T& default_value) const;
    
    // Проверка наличия ключа
    bool has(const std::string& key) const;

    // Удаление настройки
    void remove(const std::string& key);
    
    // Получение всех настроек
    const std::unordered_map<std::string, Value>& all() const { return values_; }

private:
    std::unordered_map<std::string, Value> values_;
    Logger& logger_() const;

    void setup_defaults();
    bool parse_json(const std::string& json_str);
    std::string to_json() const;
};
