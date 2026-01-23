#pragma once

#include <map>
#include <memory>
#include <vector>
#include <string>
#include <filesystem>
#include <functional>
#include <cstring>
#include <dlfcn.h>
#include <algorithm>
#include "../sdk/connector.h"
#include "../sdk/handler.h"
#include <boost/asio.hpp>

namespace fs = std::filesystem;

namespace gn {

class PluginManager {
private:
    struct HandlerInfo {
        void* dl_handle = nullptr;
        handler_t* handler = nullptr;
        fs::path path;
        std::string name;
        bool enabled = true;
        
        ~HandlerInfo();
    };
    
    struct ConnectorInfo {
        void* dl_handle = nullptr;
        connector_ops_t* ops = nullptr;
        fs::path path;
        std::string name;
        std::string scheme;
        bool enabled = true;
        
        ~ConnectorInfo();
    };
    
    host_api_t* host_api_;
    fs::path plugins_base_dir_;
    
    std::vector<std::shared_ptr<HandlerInfo>> handlers_;
    std::vector<std::shared_ptr<ConnectorInfo>> connectors_;
    std::map<std::string, std::shared_ptr<ConnectorInfo>> scheme_to_connector_;
    
    // Индексы для быстрого поиска
    std::map<std::string, std::shared_ptr<HandlerInfo>> name_to_handler_;
    
    template<typename Func>
    void safe_execute(std::string_view plugin_name, Func&& func) noexcept;
    
    bool load_handler(const fs::path& path);
    bool load_connector(const fs::path& path);
    
    void register_handler(std::shared_ptr<HandlerInfo> handler_info);
    void register_connector(std::shared_ptr<ConnectorInfo> connector_info);
    
    // Подписка обработчиков на сигналы
    void subscribe_handler_to_signals(std::shared_ptr<HandlerInfo> handler_info);
    
    // Вспомогательные методы для сканирования директорий
    std::vector<fs::path> scan_plugin_directory(const fs::path& dir, 
                                               const std::string& pattern = "*.so");
    bool validate_plugin_file(const fs::path& path);
    
public:
    PluginManager(host_api_t* api, fs::path plugins_base_dir = "");
    ~PluginManager();
    
    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;
    
    // Загрузка плагинов
    bool load_plugin(const fs::path& path);
    bool load_plugin_by_name(const std::string& name);
    
    // Загрузка из директорий
    void load_all_plugins();
    size_t load_plugins_from_directory(const fs::path& dir_path);
    size_t load_handlers_from_directory(const fs::path& dir_path);
    size_t load_connectors_from_directory(const fs::path& dir_path);
    
    // Поиск плагинов
    std::vector<std::string> find_handlers_by_type(uint32_t type) const;
    std::vector<std::string> find_handlers_by_name(const std::string& pattern) const;
    std::optional<handler_t*> find_handler_by_name(const std::string& name) const;
    std::optional<connector_ops_t*> find_connector_by_scheme(const std::string& scheme) const;
    std::optional<connector_ops_t*> find_connector_by_name(const std::string& name) const;
    
    // Управление плагинами
    bool enable_handler(const std::string& name);
    bool disable_handler(const std::string& name);
    bool enable_connector(const std::string& scheme);
    bool disable_connector(const std::string& scheme);
    
    bool unload_handler(const std::string& name);
    bool unload_connector(const std::string& scheme);
    
    // Получение информации
    size_t get_handler_count() const { return handlers_.size(); }
    size_t get_connector_count() const { return connectors_.size(); }
    size_t get_enabled_handler_count() const;
    size_t get_enabled_connector_count() const;
    
    // Вывод информации
    void list_plugins() const;
    void print_detailed_info() const;
    
    // Выгрузка
    void unload_all();
    
    // Конфигурация путей
    void set_plugins_base_dir(const fs::path& dir);
    fs::path get_plugins_base_dir() const { return plugins_base_dir_; }
    fs::path get_handlers_dir() const { return plugins_base_dir_ / "handlers"; }
    fs::path get_connectors_dir() const { return plugins_base_dir_ / "connectors"; }
};

} // namespace gn
