#include <dlfcn.h>
#include <fmt/ranges.h>
#include <iomanip>
#include <algorithm>
#include <regex>
#include <thread>
#include <chrono>

#include "pluginManager.hpp"
#include "stats.hpp"
#include "logger.hpp"
#include "signals.hpp"

namespace gn {
#define EXPECTED_API_VERSION 1

// ========== Деструкторы ==========

PluginManager::HandlerInfo::~HandlerInfo() {
    if (dl_handle) {
        // Вызываем shutdown если есть
        if (handler && handler->shutdown) {
            try {
                handler->shutdown(handler->user_data);
            } catch (const std::exception& e) {
                LOG_ERROR("Plugin '{}' error: {}", name, fmt::format("shutdown failed: {}", e.what()));
            } catch (...) {
                LOG_ERROR("Plugin '{}' error: {}", name, "shutdown failed with unknown exception");
            }
        }
        dlclose(dl_handle);
        LOG_TRACE("HandlerInfo destroyed: {}", name);
    }
}

PluginManager::ConnectorInfo::~ConnectorInfo() {
    if (dl_handle) {
        // Вызываем shutdown если есть
        if (ops && ops->shutdown) {
            try {
                ops->shutdown(ops->connector_ctx);
            } catch (const std::exception& e) {
                LOG_ERROR("Plugin '{}' error: {}", name, fmt::format("shutdown failed: {}", e.what()));
            } catch (...) {
                LOG_ERROR("Plugin '{}' error: {}", name, "shutdown failed with unknown exception");
            }
        }
        dlclose(dl_handle);
        LOG_TRACE("ConnectorInfo destroyed: {}", name);
    }
}

// ========== Конструктор/Деструктор ==========

PluginManager::PluginManager(host_api_t* api, fs::path plugins_base_dir)
    : host_api_(api), plugins_base_dir_(std::move(plugins_base_dir)) {
    
    LOG_TRACE_ENTER();
    
    if (!host_api_) {
        throw std::invalid_argument("Host API cannot be null");
    }
    
    // Если путь не указан, используем дефолтный
    if (plugins_base_dir_.empty()) {
        plugins_base_dir_ = fs::current_path() / "plugins";
    }
    
    LOG_INFO("Initialized with plugins directory: {}", plugins_base_dir_.string());
    LOG_INFO("Host API: {}", fmt::ptr(host_api_));
    
    LOG_TRACE_EXIT();
}

PluginManager::~PluginManager() {
    LOG_TRACE_ENTER();
    unload_all();
    LOG_TRACE_EXIT();
}

// ========== Вспомогательные методы ==========

template<typename Func>
void PluginManager::safe_execute(std::string_view plugin_name, Func&& func) noexcept {
    try {
        std::invoke(std::forward<Func>(func));
    } catch (const std::exception& e) {
        LOG_ERROR("Plugin '{}' error: {}", std::string(plugin_name), e.what());
    } catch (...) {
        LOG_ERROR("Plugin '{}' error: {}", std::string(plugin_name), "Unknown exception");
    }
}

// ========== Подписка на сигналы ==========

void PluginManager::subscribe_handler_to_signals(std::shared_ptr<HandlerInfo> handler_info) {
    if (!handler_info->handler) {
        return;
    }
    
    // Подписываем на сигнал пакетов
    if (handler_info->handler->handle_message) {
        packet_signal->connect([handler_info](
            const header_t* header,
            const endpoint_t* endpoint,
            std::span<const char> payload) {
            
            if (handler_info->enabled && handler_info->handler->handle_message) {
                try {
                    handler_info->handler->handle_message(
                        handler_info->handler->user_data,
                        header,
                        endpoint,
                        payload.data(),
                        payload.size()
                    );
                } catch (const std::exception& e) {
                    LOG_ERROR("Plugin '{}' error: {}", handler_info->name, e.what());
                }
            }
        });
    }
    
    // Подписываем на сигнал состояния соединений
    if (handler_info->handler->handle_conn_state) {
        conn_state_signal->connect([handler_info](
            const char* uri,
            conn_state_t state) {
            
            if (handler_info->enabled && handler_info->handler->handle_conn_state) {
                try {
                    handler_info->handler->handle_conn_state(
                        handler_info->handler->user_data,
                        uri,
                        state
                    );
                } catch (const std::exception& e) {
                    LOG_ERROR("Plugin '{}' error: {}", handler_info->name, e.what());
                }
            }
        });
    }
}

// ========== Регистрация обработчика ==========

void PluginManager::register_handler(std::shared_ptr<HandlerInfo> handler_info) {
    LOG_TRACE_ENTER_ARGS("name: {}", handler_info->name);
    
    if (!handler_info->handler) {
        LOG_ERROR("Cannot register null handler");
        LOG_TRACE_EXIT();
        return;
    }
    
    // Проверяем уникальность имени
    if (name_to_handler_.find(handler_info->name) != name_to_handler_.end()) {
        LOG_ERROR("Duplicate handler name: {}", handler_info->name);
        LOG_TRACE_EXIT();
        return;
    }
    
    // Добавляем в список всех обработчиков
    handlers_.push_back(handler_info);
    name_to_handler_[handler_info->name] = handler_info;
    Stats::total_handlers++;
    Stats::enabled_handlers++;
    Stats::loaded_handlers.push_back(handler_info->name);
    
    // Подписываем на сигналы
    subscribe_handler_to_signals(handler_info);
    
    LOG_INFO("Registered handler: {}", handler_info->name);
    
    LOG_TRACE_EXIT();
}

// ========== Регистрация коннектора ==========

void PluginManager::register_connector(std::shared_ptr<ConnectorInfo> connector_info) {
    LOG_TRACE_ENTER_ARGS("name: {}", connector_info->name);
    
    if (!connector_info->ops) {
        LOG_ERROR("Cannot register null connector");
        LOG_TRACE_EXIT();
        return;
    }
    
    // Получаем схему протокола
    char scheme_buffer[64] = {0};
    if (connector_info->ops->get_scheme) {
        connector_info->ops->get_scheme(
            connector_info->ops->connector_ctx,
            scheme_buffer,
            sizeof(scheme_buffer)
        );
    }
    
    std::string scheme(scheme_buffer);
    if (scheme.empty()) {
        LOG_ERROR("Connector has empty scheme");
        LOG_TRACE_EXIT();
        return;
    }
    
    // Проверяем уникальность схемы
    if (scheme_to_connector_.find(scheme) != scheme_to_connector_.end()) {
        LOG_ERROR("Duplicate connector scheme: {}", scheme);
        LOG_TRACE_EXIT();
        return;
    }
    
    connector_info->scheme = scheme;
    scheme_to_connector_[scheme] = connector_info;
    
    // Получаем имя
    char name_buffer[128] = {0};
    if (connector_info->ops->get_name) {
        connector_info->ops->get_name(
            connector_info->ops->connector_ctx,
            name_buffer,
            sizeof(name_buffer)
        );
    }
    
    connector_info->name = name_buffer[0] ? name_buffer : "Unnamed Connector";
    
    // Добавляем в список всех коннекторов
    connectors_.push_back(connector_info);
    Stats::total_connectors++;
    Stats::enabled_connectors++;
    Stats::loaded_connectors.push_back(fmt::format("{} ({})", connector_info->name, scheme));

    LOG_INFO("Registered connector: {} (scheme: {})", connector_info->name, scheme);
    
    LOG_TRACE_EXIT();
}

// ========== Загрузка обработчика ==========

bool PluginManager::load_handler(const fs::path& path) {
    LOG_TRACE_ENTER_ARGS("path: {}", path.string());
    
    if (path.empty() || !fs::exists(path)) {
        LOG_WARN("Invalid path: {}", path.string());
        LOG_TRACE_EXIT_VALUE(false);
        return false;
    }
    
    auto start_time = std::chrono::high_resolution_clock::now();

    // Открываем библиотеку
    void* lib_handle = dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL | RTLD_DEEPBIND);
    if (!lib_handle) {
        const char* error = dlerror();
        LOG_WARN("dlopen failed: {}", error ? error : "Unknown error");
        LOG_TRACE_EXIT_VALUE(false);
        return false;
    }
    
    // Сбрасываем ошибки
    dlerror();
    
    // Получаем указатель на функцию
    handler_init_t init_func = (handler_init_t)dlsym(lib_handle, "handler_init");
    
    const char* dlsym_error = dlerror();
    if (dlsym_error) {
        LOG_WARN("dlsym failed: {}", dlsym_error);
        dlclose(lib_handle);
        LOG_TRACE_EXIT_VALUE(false);
        return false;
    }
    
    if (!init_func) {
        LOG_WARN("Function pointer is null!");
        dlclose(lib_handle);
        LOG_TRACE_EXIT_VALUE(false);
        return false;
    }
    
    // Проверяем версию API
    if (host_api_->api_version != EXPECTED_API_VERSION) {
        LOG_WARN("API version mismatch: plugin expects {}, host provides {}", 
                      EXPECTED_API_VERSION, host_api_->api_version);
    }
    
    // Устанавливаем тип плагина
    host_api_->plugin_type = PLUGIN_TYPE_HANDLER;
    
    // Вызываем инициализацию
    handler_t* handler = nullptr;
    int result = init_func(host_api_, &handler);
    
    LOG_INFO("handler_init returned: {}, handler ptr: {}", result, fmt::ptr(handler));
    
    if (result != 0 || !handler) {
        LOG_WARN("Handler initialization failed");
        dlclose(lib_handle);
        LOG_TRACE_EXIT_VALUE(false);
        return false;
    }
    
    // Создаем информацию об обработчике
    auto handler_info = std::make_shared<HandlerInfo>();
    handler_info->path = path;
    handler_info->name = path.stem().string();
    handler_info->handler = handler;
    handler_info->dl_handle = lib_handle;
    handler_info->enabled = true;
    
    // Регистрируем
    register_handler(handler_info);
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    Stats::load_times.push_back(duration.count());
    
    LOG_INFO("Handler loaded in {} ms: {}", duration.count(), path.string());
    LOG_TRACE_EXIT_VALUE(true);
    return true;
}

// ========== Загрузка коннектора ==========

bool PluginManager::load_connector(const fs::path& path) {
    LOG_TRACE_ENTER_ARGS("path: {}", path.string());
    
    if (path.empty() || !fs::exists(path)) {
        LOG_WARN("Invalid path: {}", path.string());
        LOG_TRACE_EXIT_VALUE(false);
        return false;
    }
    
    auto start_time = std::chrono::high_resolution_clock::now();

    // Открываем библиотеку
    void* lib_handle = dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL | RTLD_DEEPBIND);
    if (!lib_handle) {
        const char* error = dlerror();
        LOG_WARN("dlopen failed: {}", error ? error : "Unknown error");
        LOG_TRACE_EXIT_VALUE(false);
        return false;
    }
    
    // Сбрасываем ошибки
    dlerror();
    
    // Получаем указатель на функцию
    connector_init_t init_func = (connector_init_t)dlsym(lib_handle, "connector_init");
    
    const char* dlsym_error = dlerror();
    if (dlsym_error) {
        LOG_WARN("dlsym failed: {}", dlsym_error);
        dlclose(lib_handle);
        LOG_TRACE_EXIT_VALUE(false);
        return false;
    }
    
    if (!init_func) {
        LOG_WARN("Function pointer is null!");
        dlclose(lib_handle);
        LOG_TRACE_EXIT_VALUE(false);
        return false;
    }
    
    // Проверяем версию API
    if (host_api_->api_version != EXPECTED_API_VERSION) {
        LOG_WARN("API version mismatch: plugin expects {}, host provides {}", 
                      EXPECTED_API_VERSION, host_api_->api_version);
    }
    
    // Устанавливаем тип плагина
    host_api_->plugin_type = PLUGIN_TYPE_CONNECTOR;
    
    // Вызываем инициализацию
    connector_ops_t* ops = nullptr;
    int result = init_func(host_api_, &ops);
    
    LOG_INFO("connector_init returned: {}, ops ptr: {}", result, fmt::ptr(ops));
    
    if (result != 0 || !ops) {
        LOG_WARN("Connector initialization failed");
        dlclose(lib_handle);
        LOG_TRACE_EXIT_VALUE(false);
        return false;
    }
    
    // Проверяем обязательные функции
    if (!ops->connect || !ops->get_scheme) {
        LOG_WARN("Connector missing required functions");
        dlclose(lib_handle);
        LOG_TRACE_EXIT_VALUE(false);
        return false;
    }
    
    // Создаем информацию о коннекторе
    auto connector_info = std::make_shared<ConnectorInfo>();
    connector_info->path = path;
    connector_info->name = path.stem().string();
    connector_info->ops = ops;
    connector_info->dl_handle = lib_handle;
    connector_info->enabled = true;
    
    // Регистрируем
    register_connector(connector_info);
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    Stats::load_times.push_back(duration.count());
    
    LOG_INFO("Connector loaded in {} ms: {}", duration.count(), path.string());
    LOG_TRACE_EXIT_VALUE(true);
    return true;
}

// ========== Основная загрузка плагина ==========

bool PluginManager::load_plugin(const fs::path& path) {
    LOG_TRACE_ENTER_ARGS("path: {}", path.string());
    
    // Проверка существования файла
    if (!fs::exists(path)) {
        LOG_WARN("Plugin file not found: {}", path.string());
        LOG_TRACE_EXIT_VALUE(false);
        return false;
    }
    
    // Определяем тип по пути
    std::string path_str = path.string();
    if (path_str.find("handlers") != std::string::npos) {
        LOG_INFO("Detected as handler by path");
        bool result = load_handler(path);
        LOG_TRACE_EXIT_VALUE(result);
        return result;
    }
    else if (path_str.find("connectors") != std::string::npos) {
        LOG_INFO("Detected as connector by path");
        bool result = load_connector(path);
        LOG_TRACE_EXIT_VALUE(result);
        return result;
    }
    else {
        // Пробуем оба варианта
        LOG_INFO("Unknown path, trying handler first...");
        if (load_handler(path)) {
            LOG_TRACE_EXIT_VALUE(true);
            return true;
        }
        LOG_INFO("Not a handler, trying connector...");
        bool result = load_connector(path);
        LOG_TRACE_EXIT_VALUE(result);
        return result;
    }
}

// ========== Сканирование директории плагинов ==========

std::vector<fs::path> PluginManager::scan_plugin_directory(const fs::path& dir, 
                                                          const std::string& pattern) {
    LOG_TRACE_ENTER_ARGS("dir: {}", dir.string());
    
    std::vector<fs::path> result;
    
    try {
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();
                std::string ext = entry.path().extension().string();
                
                // Проверяем расширение .so (или .dll на Windows)
                if (ext == ".so" || ext == ".dll") {
                    if (validate_plugin_file(entry.path())) {
                        result.push_back(entry.path());
                    }
                }
            }
        }
    } catch (const fs::filesystem_error& e) {
        LOG_ERROR("Error scanning directory {}: {}", dir.string(), e.what());
    }
    
    // Сортируем по алфавиту для предсказуемости загрузки
    std::sort(result.begin(), result.end());
    
    LOG_TRACE_EXIT_VALUE(result.size());
    return result;
}

bool PluginManager::validate_plugin_file(const fs::path& path) {
    // Проверяем существование файла
    if (!fs::exists(path)) {
        return false;
    }
    
    // Проверяем размер файла (не менее 1KB и не более 100MB)
    auto file_size = fs::file_size(path);
    if (file_size < 1024 || file_size > 100 * 1024 * 1024) {
        LOG_WARN("Plugin file size suspicious: {} ({} bytes)", 
                       path.filename().string(), file_size);
        return false;
    }
    
    // Проверяем права доступа (только для Unix)
    #ifdef __unix__
    fs::perms p = fs::status(path).permissions();
    if ((p & fs::perms::owner_exec) == fs::perms::none) {
        LOG_WARN("Plugin file is not executable: {}", path.filename().string());
        return false;
    }
    #endif
    
    return true;
}

// ========== Загрузка плагинов по имени ==========

bool PluginManager::load_plugin_by_name(const std::string& name) {
    LOG_TRACE_ENTER_ARGS("name: {}", name);
    
    // Пробуем найти в handlers
    fs::path handler_path = get_handlers_dir() / (name + ".so");
    if (fs::exists(handler_path)) {
        bool result = load_handler(handler_path);
        LOG_TRACE_EXIT_VALUE(result);
        return result;
    }
    
    // Пробуем найти в connectors
    fs::path connector_path = get_connectors_dir() / (name + ".so");
    if (fs::exists(connector_path)) {
        bool result = load_connector(connector_path);
        LOG_TRACE_EXIT_VALUE(result);
        return result;
    }
    
    // Пробуем найти в корневой директории
    fs::path root_path = plugins_base_dir_ / (name + ".so");
    if (fs::exists(root_path)) {
        bool result = load_plugin(root_path);
        LOG_TRACE_EXIT_VALUE(result);
        return result;
    }
    
    LOG_WARN("Plugin not found: {}", name);
    LOG_TRACE_EXIT_VALUE(false);
    return false;
}

// ========== Загрузка из директорий ==========

size_t PluginManager::load_plugins_from_directory(const fs::path& dir_path) {
    LOG_TRACE_ENTER_ARGS("dir_path: {}", dir_path.string());
    
    if (!fs::exists(dir_path) || !fs::is_directory(dir_path)) {
        LOG_ERROR("Plugins directory not found: {}", dir_path.string());
        LOG_TRACE_EXIT_VALUE(0);
        return 0;
    }
    
    LOG_INFO("Loading all plugins from: {}", dir_path.string());
    
    auto plugin_files = scan_plugin_directory(dir_path);
    size_t loaded_count = 0;
    
    for (const auto& file : plugin_files) {
        if (load_plugin(file)) {
            loaded_count++;
        }
    }
    
    LOG_INFO("Loaded {} plugins from {}", loaded_count, dir_path.string());
    
    LOG_TRACE_EXIT_VALUE(loaded_count);
    return loaded_count;
}

size_t PluginManager::load_handlers_from_directory(const fs::path& dir_path) {
    LOG_TRACE_ENTER_ARGS("dir_path: {}", dir_path.string());
    
    if (!fs::exists(dir_path) || !fs::is_directory(dir_path)) {
        LOG_DEBUG("Handlers directory not found: {}", dir_path.string());
        LOG_TRACE_EXIT_VALUE(0);
        return 0;
    }
    
    LOG_INFO("Loading handlers from: {}", dir_path.string());
    
    auto plugin_files = scan_plugin_directory(dir_path);
    size_t loaded_count = 0;
    
    for (const auto& file : plugin_files) {
        if (load_handler(file)) {
            loaded_count++;
        }
    }
    
    LOG_TRACE_EXIT_VALUE(loaded_count);
    return loaded_count;
}

size_t PluginManager::load_connectors_from_directory(const fs::path& dir_path) {
    LOG_TRACE_ENTER_ARGS("dir_path: {}", dir_path.string());
    
    if (!fs::exists(dir_path) || !fs::is_directory(dir_path)) {
        LOG_DEBUG("Connectors directory not found: {}", dir_path.string());
        LOG_TRACE_EXIT_VALUE(0);
        return 0;
    }
    
    LOG_INFO("Loading connectors from: {}", dir_path.string());
    
    auto plugin_files = scan_plugin_directory(dir_path);
    size_t loaded_count = 0;
    
    for (const auto& file : plugin_files) {
        if (load_connector(file)) {
            loaded_count++;
        }
    }
    
    LOG_TRACE_EXIT_VALUE(loaded_count);
    return loaded_count;
}

void PluginManager::load_all_plugins() {
    LOG_TRACE_ENTER();
    
    LOG_INFO("Loading all plugins from base directory: {}", plugins_base_dir_.string());
    
    // Создаем директории, если их нет
    if (!fs::exists(plugins_base_dir_)) {
        LOG_INFO("Creating plugins directory structure...");
        fs::create_directories(get_handlers_dir());
        fs::create_directories(get_connectors_dir());
    }
    
    size_t total_loaded = 0;
    
    // Загружаем обработчики
    size_t handlers_loaded = load_handlers_from_directory(get_handlers_dir());
    total_loaded += handlers_loaded;
    
    // Загружаем коннекторы
    size_t connectors_loaded = load_connectors_from_directory(get_connectors_dir());
    total_loaded += connectors_loaded;
    
    LOG_INFO("Total plugins loaded: {} ({} handlers, {} connectors)",
                total_loaded, handlers_loaded, connectors_loaded);
    
    LOG_TRACE_EXIT_VALUE(total_loaded);
}

// ========== Поиск плагинов ==========

std::vector<std::string> PluginManager::find_handlers_by_type(uint32_t type) const {
    LOG_TRACE_ENTER_ARGS("type: {}", type);
    
    std::vector<std::string> result;
    for (const auto& handler : handlers_) {
        if (handler->enabled && handler->handler) {
            // Проверяем, поддерживает ли обработчик данный тип
            if (handler->handler->num_supported_types == 0) {
                // Если не указаны типы, значит поддерживает все
                result.push_back(handler->name);
            } else {
                for (size_t i = 0; i < handler->handler->num_supported_types; ++i) {
                    if (handler->handler->supported_types[i] == type) {
                        result.push_back(handler->name);
                        break;
                    }
                }
            }
        }
    }
    
    LOG_TRACE_EXIT_VALUE(result.size());
    return result;
}

std::vector<std::string> PluginManager::find_handlers_by_name(const std::string& pattern) const {
    LOG_TRACE_ENTER_ARGS("pattern: {}", pattern);
    
    std::vector<std::string> result;
    std::regex regex_pattern(pattern, std::regex::icase);
    
    for (const auto& handler : handlers_) {
        if (handler->enabled && std::regex_search(handler->name, regex_pattern)) {
            result.push_back(handler->name);
        }
    }
    
    LOG_TRACE_EXIT_VALUE(result.size());
    return result;
}

std::optional<handler_t*> PluginManager::find_handler_by_name(const std::string& name) const {
    LOG_TRACE_ENTER_ARGS("name: {}", name);
    
    auto it = name_to_handler_.find(name);
    if (it != name_to_handler_.end() && it->second->enabled) {
        LOG_TRACE_EXIT_VALUE(fmt::ptr(it->second->handler));
        return it->second->handler;
    }
    
    LOG_TRACE_EXIT_VALUE("nullopt");
    return std::nullopt;
}

std::optional<connector_ops_t*> PluginManager::find_connector_by_scheme(const std::string& scheme) const {
    LOG_TRACE_ENTER_ARGS("scheme: {}", scheme);
    
    auto it = scheme_to_connector_.find(scheme);
    if (it != scheme_to_connector_.end() && it->second->enabled) {
        LOG_TRACE_EXIT_VALUE(fmt::ptr(it->second->ops));
        return it->second->ops;
    }
    
    LOG_TRACE_EXIT_VALUE("nullopt");
    return std::nullopt;
}

std::optional<connector_ops_t*> PluginManager::find_connector_by_name(const std::string& name) const {
    LOG_TRACE_ENTER_ARGS("name: {}", name);
    
    for (const auto& connector : connectors_) {
        if (connector->enabled && connector->name == name) {
            LOG_TRACE_EXIT_VALUE(fmt::ptr(connector->ops));
            return connector->ops;
        }
    }
    
    LOG_TRACE_EXIT_VALUE("nullopt");
    return std::nullopt;
}

// ========== Управление плагинами ==========

bool PluginManager::enable_handler(const std::string& name) {
    LOG_TRACE_ENTER_ARGS("name: {}", name);
    
    auto it = name_to_handler_.find(name);
    if (it != name_to_handler_.end()) {
        if (!it->second->enabled) {
            it->second->enabled = true;
            Stats::enabled_handlers++;
            LOG_INFO("Handler enabled: {}", name);
            LOG_TRACE_EXIT_VALUE(true);
            return true;
        }
    }
    
    LOG_TRACE_EXIT_VALUE(false);
    return false;
}

bool PluginManager::disable_handler(const std::string& name) {
    LOG_TRACE_ENTER_ARGS("name: {}", name);
    
    auto it = name_to_handler_.find(name);
    if (it != name_to_handler_.end()) {
        if (it->second->enabled) {
            it->second->enabled = false;
            Stats::enabled_handlers--;
            LOG_INFO("Handler disabled: {}", name);
            LOG_TRACE_EXIT_VALUE(true);
            return true;
        }
    }
    
    LOG_TRACE_EXIT_VALUE(false);
    return false;
}

bool PluginManager::enable_connector(const std::string& scheme) {
    LOG_TRACE_ENTER_ARGS("scheme: {}", scheme);
    
    auto it = scheme_to_connector_.find(scheme);
    if (it != scheme_to_connector_.end()) {
        if (!it->second->enabled) {
            it->second->enabled = true;
            Stats::enabled_connectors++;
            LOG_INFO("Connector enabled: {} (scheme: {})", 
                        it->second->name, scheme);
            LOG_TRACE_EXIT_VALUE(true);
            return true;
        }
    }
    
    LOG_TRACE_EXIT_VALUE(false);
    return false;
}

bool PluginManager::disable_connector(const std::string& scheme) {
    LOG_TRACE_ENTER_ARGS("scheme: {}", scheme);
    
    auto it = scheme_to_connector_.find(scheme);
    if (it != scheme_to_connector_.end()) {
        if (it->second->enabled) {
            it->second->enabled = false;
            Stats::enabled_connectors--;
            LOG_INFO("Connector disabled: {} (scheme: {})", 
                        it->second->name, scheme);
            LOG_TRACE_EXIT_VALUE(true);
            return true;
        }
    }
    
    LOG_TRACE_EXIT_VALUE(false);
    return false;
}

bool PluginManager::unload_handler(const std::string& name) {
    LOG_TRACE_ENTER_ARGS("name: {}", name);
    
    auto it = name_to_handler_.find(name);
    if (it == name_to_handler_.end()) {
        LOG_TRACE_EXIT_VALUE(false);
        return false;
    }
    
    auto handler_info = it->second;
    
    // Вызываем shutdown если есть
    if (handler_info->handler && handler_info->handler->shutdown) {
        safe_execute(name, [&]() {
            handler_info->handler->shutdown(handler_info->handler->user_data);
        });
    }
    
    // Удаляем из мапы
    name_to_handler_.erase(it);
    
    // Удаляем из вектора
    handlers_.erase(
        std::remove_if(handlers_.begin(), handlers_.end(),
            [name](const auto& h) { return h->name == name; }),
        handlers_.end()
    );
    
    // Обновляем статистику
    Stats::total_handlers--;
    if (handler_info->enabled) {
        Stats::enabled_handlers--;
    }
    // Удаляем из loaded_handlers
    auto& loaded = Stats::loaded_handlers;
    loaded.erase(std::remove(loaded.begin(), loaded.end(), name), loaded.end());
    
    LOG_INFO("Handler unloaded: {}", name);
    LOG_TRACE_EXIT_VALUE(true);
    return true;
}

bool PluginManager::unload_connector(const std::string& scheme) {
    LOG_TRACE_ENTER_ARGS("scheme: {}", scheme);
    
    auto it = scheme_to_connector_.find(scheme);
    if (it == scheme_to_connector_.end()) {
        LOG_TRACE_EXIT_VALUE(false);
        return false;
    }
    
    auto connector_info = it->second;
    std::string name = connector_info->name;
    
    // Вызываем shutdown если есть
    if (connector_info->ops && connector_info->ops->shutdown) {
        safe_execute(name, [&]() {
            connector_info->ops->shutdown(connector_info->ops->connector_ctx);
        });
    }
    
    // Удаляем из мапы
    scheme_to_connector_.erase(it);
    
    // Удаляем из вектора
    connectors_.erase(
        std::remove_if(connectors_.begin(), connectors_.end(),
            [scheme](const auto& c) { return c->scheme == scheme; }),
        connectors_.end()
    );
    
    // Обновляем статистику
    Stats::total_connectors--;
    if (connector_info->enabled) {
        Stats::enabled_connectors--;
    }
    // Удаляем из loaded_connectors
    auto& loaded = Stats::loaded_connectors;
    std::string connector_id = fmt::format("{} ({})", name, scheme);
    loaded.erase(std::remove(loaded.begin(), loaded.end(), connector_id), loaded.end());
    
    LOG_INFO("Connector unloaded: {} (scheme: {})", name, scheme);
    LOG_TRACE_EXIT_VALUE(true);
    return true;
}

// ========== Статистика ==========

size_t PluginManager::get_enabled_handler_count() const {
    LOG_TRACE_ENTER();
    size_t count = std::count_if(handlers_.begin(), handlers_.end(),
                                [](const auto& h) { return h->enabled; });
    LOG_TRACE_EXIT_VALUE(count);
    return count;
}

size_t PluginManager::get_enabled_connector_count() const {
    LOG_TRACE_ENTER();
    size_t count = std::count_if(connectors_.begin(), connectors_.end(),
                                [](const auto& c) { return c->enabled; });
    LOG_TRACE_EXIT_VALUE(count);
    return count;
}

// ========== Вывод информации ==========

void PluginManager::list_plugins() const {
    LOG_TRACE_ENTER();
    
    fmt::memory_buffer buffer;
    
    // Форматируем список обработчиков (Handlers)
    std::vector<std::string> handler_strings;
    for (const auto& h : handlers_) {
        std::string status = h->enabled ? "✓" : "✗";
        handler_strings.push_back(
            fmt::format("  {} {:<20} | {}", 
                       status, 
                       h->name.size() > 20 ? h->name.substr(0, 17) + "..." : h->name,
                       h->path.filename().string())
        );
    }

    // Форматируем список коннекторов (Connectors)
    std::vector<std::string> connector_strings;
    for (const auto& c : connectors_) {
        std::string status = c->enabled ? "✓" : "✗";
        connector_strings.push_back(
            fmt::format("  {} {:<15} | scheme: {:<8} | {}", 
                       status, 
                       c->name.size() > 15 ? c->name.substr(0, 12) + "..." : c->name,
                       c->scheme.size() > 8 ? c->scheme.substr(0, 5) + "..." : c->scheme,
                       c->path.filename().string())
        );
    }

    // Формируем весь вывод в буфере
    fmt::format_to(std::back_inserter(buffer),
        "\n┌────────────────── Loaded Plugins ──────────────────┐\n"
        "│ Handlers ({} enabled / {} total):\n",
        get_enabled_handler_count(), handlers_.size());
    
    if (handler_strings.empty()) {
        fmt::format_to(std::back_inserter(buffer), "  (none)\n");
    } else {
        fmt::format_to(std::back_inserter(buffer), "{}\n", 
                      fmt::join(handler_strings, "\n"));
    }
    
    fmt::format_to(std::back_inserter(buffer),
        "│ Connectors ({} enabled / {} total):\n",
        get_enabled_connector_count(), connectors_.size());
    
    if (connector_strings.empty()) {
        fmt::format_to(std::back_inserter(buffer), "  (none)\n");
    } else {
        fmt::format_to(std::back_inserter(buffer), "{}\n", 
                      fmt::join(connector_strings, "\n"));
    }
    
    fmt::format_to(std::back_inserter(buffer),
        "└────────────────────────────────────────────────────┘");
    
    // Выводим одним вызовом
    LOG_INFO("{}", std::string_view(buffer.data(), buffer.size()));
    
    LOG_TRACE_EXIT();
}

void PluginManager::print_detailed_info() const {
    LOG_TRACE_ENTER();
    
    // Формируем весь вывод в буфер
    fmt::memory_buffer buffer;
    
    // Верхняя граница
    fmt::format_to(std::back_inserter(buffer),
        "\n╔══════════════════════════════════════════════════════════╗\n"
        "║                  Plugin Manager Status                   ║\n"
        "╠══════════════════════════════════════════════════════════╣\n"
        "║  Base Directory: {:<39} ║\n"
        "╠══════════════════════════════════════════════════════════╣\n"
        "║  Handlers:       {:>5}/{:<5} enabled                     ║\n"
        "║  Connectors:     {:>5}/{:<5} enabled                     ║\n",
        plugins_base_dir_.string().substr(0, 38),  // Ограничиваем длину пути
        get_enabled_handler_count(), handlers_.size(),
        get_enabled_connector_count(), connectors_.size());
    
    if (!Stats::loaded_handlers.empty()) {
        fmt::format_to(std::back_inserter(buffer),
            "╠══════════════════════════════════════════════════════════╣\n"
            "║  Loaded Handlers:                                        ║\n");
        for (const auto& name : Stats::loaded_handlers) {
            fmt::format_to(std::back_inserter(buffer),
                "║    • {:<51} ║\n", 
                name.size() > 46 ? name.substr(0, 43) + "..." : name);
        }
    }
    
    if (!Stats::loaded_connectors.empty()) {
        fmt::format_to(std::back_inserter(buffer),
            "╠══════════════════════════════════════════════════════════╣\n"
            "║  Loaded Connectors:                                      ║\n");
        for (const auto& info : Stats::loaded_connectors) {
            fmt::format_to(std::back_inserter(buffer),
                "║    • {:<51} ║\n",
                info.size() > 46 ? info.substr(0, 43) + "..." : info);
        }
    }
    
    // Нижняя граница
    fmt::format_to(std::back_inserter(buffer),
        "╚══════════════════════════════════════════════════════════╝\n");
    
    // Выводим одним вызовом
    LOG_INFO("{}", std::string_view(buffer.data(), buffer.size()));
    
    LOG_TRACE_EXIT();
}

// ========== Выгрузка всех плагинов ==========

void PluginManager::unload_all() {
    LOG_TRACE_ENTER();
    
    LOG_INFO("Unloading all plugins...");
    
    // Создаем копии имен
    std::vector<std::string> handler_names;
    for (const auto& h : handlers_) {
        handler_names.push_back(h->name);
    }
    
    std::vector<std::string> connector_schemes;
    for (const auto& c : connectors_) {
        connector_schemes.push_back(c->scheme);
    }
    
    // Выгружаем все обработчики
    for (const auto& name : handler_names) {
        unload_handler(name);
    }
    
    // Выгружаем все коннекторы
    for (const auto& scheme : connector_schemes) {
        unload_connector(scheme);
    }
    
    LOG_INFO("All plugins unloaded");
    LOG_TRACE_EXIT();
}

// ========== Конфигурация путей ==========

void PluginManager::set_plugins_base_dir(const fs::path& dir) {
    LOG_TRACE_ENTER_ARGS("dir: {}", dir.string());
    
    plugins_base_dir_ = dir;
    LOG_INFO("Plugins base directory changed to: {}", plugins_base_dir_.string());
    
    LOG_TRACE_EXIT();
}

} // namespace gn