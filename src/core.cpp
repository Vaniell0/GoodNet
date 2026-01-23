#include "core.hpp"
#include "logger.hpp"
#include "pluginManager.hpp"
#include "connectManager.hpp"
#include "homeServices.hpp"

#include <boost/asio.hpp>

namespace gn {

Core::Core(Config& config)
    : config_(config),
      io_context_(config.get_or<int>("core.io_threads", 4)),
      work_guard_(boost::asio::make_work_guard(io_context_)) {
    
    LOG_INFO("Core initializing...");
    
    try {
        // 1. Инициализация глобальных сигналов
        packet_signal = std::make_shared<PacketSignal>(io_context_);
        conn_state_signal = std::make_shared<ConnStateSignal>(io_context_);
        
        // 2. Инициализация Host API
        host_api_ = std::make_unique<host_api_t>();
        initialize_host_api();
        
        // 3. Инициализация PluginManager
        auto plugins_dir = config.get_or<std::string>("plugins.base_dir", "./plugins");
        plugin_manager_ = std::make_unique<PluginManager>(host_api_.get(), plugins_dir);
        
        // 4. Инициализация ConnectManager
        connect_manager_ = std::make_unique<ConnectManager>(io_context_);
        
        // 5. Инициализация HomeServices
        home_services_ = std::make_unique<HomeServices>(io_context_, plugin_manager_);
        
        LOG_INFO("Core initialized successfully");
        
    } catch (const std::exception& e) {
        LOG_CRITICAL("Core initialization failed: {}", e.what());
        throw;
    }
}

Core::~Core() {
    LOG_INFO("Core shutting down...");
    stop();
    
    // Уничтожаем в правильном порядке
    home_services_.reset();
    connect_manager_.reset();
    plugin_manager_.reset();
    packet_signal.reset();
    conn_state_signal.reset();
    host_api_.reset();
    work_guard_.reset();
    
    LOG_INFO("Core shutdown complete");
}

void Core::initialize_host_api() {
    host_api_->api_version = GNET_API_VERSION;
    host_api_->send = &c_api_send;
    host_api_->create_connection = &c_api_create_connection;
    host_api_->close_connection = &c_api_close_connection;
    host_api_->update_connection_state = &c_api_update_connection_state;
    host_api_->plugin_type = PLUGIN_TYPE_UNKNOWN;
    
    LOG_INFO("Host API initialized with version: {}", host_api_->api_version);
}

bool Core::start() {
    if (is_running_) {
        LOG_WARN("Core already running");
        return true;
    }
    
    try {
        LOG_INFO("Starting Core...");
        
        // 1. Запускаем IO потоки
        initialize_io_threads();
        
        // 2. Загружаем плагины
        if (config_.get_or<bool>("plugins.auto_load", true)) {
            LOG_INFO("Auto-loading plugins...");
            plugin_manager_->load_all_plugins();
        }
        
        // 3. Запускаем HomeServices
        auto listen_address = config_.get_or<std::string>("core.listen_address", "0.0.0.0");
        auto listen_port = static_cast<uint16_t>(config_.get_or<int>("core.listen_port", 25565));
        
        home_services_->start(listen_address, listen_port);
        
        // 4. Инициализируем остальные компоненты
        initialize_components();
        
        is_running_ = true;
        LOG_INFO("Core started successfully");
        return true;
        
    } catch (const std::exception& e) {
        LOG_CRITICAL("Failed to start Core: {}", e.what());
        return false;
    }
}

void Core::stop() {
    if (!is_running_) {
        return;
    }
    
    LOG_INFO("Stopping Core...");
    
    // Останавливаем в обратном порядке
    if (home_services_) {
        home_services_->stop();
    }
    
    cleanup();
    
    is_running_ = false;
    LOG_INFO("Core stopped");
}

void Core::initialize_components() {
    LOG_INFO("Initializing Core components...");
    // TODO: Инициализация дополнительных компонентов
}

void Core::initialize_io_threads() {
    LOG_INFO("Starting IO threads...");
    
    unsigned int thread_count = config_.get_or<int>("core.io_threads", 4);
    thread_count = std::max(1u, thread_count);
    
    for (unsigned int i = 0; i < thread_count; ++i) {
        io_threads_.emplace_back([this, i]() {
            try {
                LOG_DEBUG("IO thread {} started", i);
                io_context_.run();
                LOG_DEBUG("IO thread {} stopped", i);
            } catch (const std::exception& e) {
                LOG_ERROR("IO thread {} error: {}", i, e.what());
            }
        });
    }
    
    LOG_INFO("{} IO threads started", thread_count);
}

void Core::cleanup() {
    // Останавливаем IO context
    io_context_.stop();
    
    // Ждем завершения потоков
    for (auto& thread : io_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    io_threads_.clear();
    
    LOG_INFO("Core cleanup complete");
}

// Реализации callback-ов
void Core::send_impl(const char* uri, uint32_t type, const void* data, size_t size) {
    LOG_DEBUG("send_impl: uri={}, type={}, size={}", 
              uri ? uri : "null", type, size);
    
    // TODO: Реализовать отправку через ConnectManager
}

handle_t Core::create_connection_impl(const char* uri) {
    if (!uri) {
        LOG_ERROR("create_connection_impl: URI is null");
        return 0;
    }
    
    LOG_INFO("create_connection_impl: uri={}", uri);
    
    // Проверяем схему URI
    std::string uri_str(uri);
    if (uri_str.find("tcp://") == 0) {
        // Для TCP соединений используем HomeServices
        return 0; // TODO: Реализовать
    } else {
        // Ищем коннектор по схеме
        auto connector = plugin_manager_->find_connector_by_scheme(
            uri_str.substr(0, uri_str.find(':'))
        );
        
        if (connector.has_value() && connector.value()) {
            // Создаем соединение через плагин
            auto connection_ops = connector.value()->connect(
                connector.value()->connector_ctx, uri
            );
            
            if (connection_ops) {
                // Регистрируем в ConnectManager
                auto handle = connect_manager_->create_connection(uri_str);
                return handle;
            }
        }
    }
    
    return 0;
}

void Core::close_connection_impl(handle_t handle) {
    LOG_INFO("close_connection_impl: handle={}", handle);
    
    if (connect_manager_) {
        connect_manager_->close_connection(handle);
    }
}

void Core::update_connection_state_impl(const char* uri, conn_state_t state) {
    LOG_INFO("update_connection_state_impl: uri={}, state={}", 
             uri ? uri : "null", static_cast<int>(state));
}

// C API static методы
void Core::c_api_send(const char* uri, uint32_t type, const void* data, size_t size) {
    // TODO: Нужен доступ к экземпляру Core
    LOG_DEBUG("c_api_send called");
}

handle_t Core::c_api_create_connection(const char* uri) {
    // TODO: Нужен доступ к экземпляру Core
    LOG_DEBUG("c_api_create_connection called");
    return 0;
}

void Core::c_api_close_connection(handle_t handle) {
    // TODO: Нужен доступ к экземпляру Core
    LOG_DEBUG("c_api_close_connection called");
}

void Core::c_api_update_connection_state(const char* uri, conn_state_t state) {
    // TODO: Нужен доступ к экземпляру Core
    LOG_DEBUG("c_api_update_connection_state called");
}

} // namespace gn