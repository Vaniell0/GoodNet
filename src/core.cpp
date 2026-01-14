#include "core.hpp"
#include "logger.hpp"

#include "stats.hpp"
#include "pluginManager.hpp"
#include "connectManager.hpp"
#include "homeServices.hpp"

namespace gn {

Core::Core(const Config& config)
    : listen_address_(config.get_or<std::string>("core.listen_address", "0.0.0.0")),
      listen_port_(static_cast<uint16_t>(config.get_or<int>("core.listen_port", 25565))),
      io_thread_count_(config.get_or<int>("core.io_threads", 4)),
      io_context_(io_thread_count_),
      work_guard_(boost::asio::make_work_guard(io_context_)),
      host_api_(std::make_unique<host_api_t>()) {
    
    if (instance_ != nullptr) {
        throw std::runtime_error("Core instance already exists");
    }

    LOG_INFO("Core initializing...");
    LOG_INFO("Listen address: {}:{}", listen_address_, listen_port_);
    LOG_INFO("IO threads: {}", io_thread_count_);
    
    try {
        // 1. Инициализация Host API
        init_host_api();
        
        // 2. Инициализация сигналов
        packet_signal_ = std::make_unique<PacketSignal>(io_context_);
        conn_state_signal_ = std::make_unique<ConnStateSignal>(io_context_);
        
        // 3. Инициализация менеджера плагинов
        auto plugins_dir = config.get_or<std::string>("plugins.base_dir", "./logs");
        plugin_manager_ = std::make_unique<PluginManager>(host_api_.get(), plugins_dir);
        
        // 4. Установка колбэков для менеджера плагинов
        plugin_manager_->set_handler_loaded_callback([this](handler_t* handler) {
            LOG_INFO("Handler loaded callback: {}", handler ? "valid" : "null");
        });
        
        plugin_manager_->set_connector_loaded_callback([this](connector_ops_t* ops) {
            LOG_INFO("Connector loaded callback: {}", ops ? "valid" : "null");
        });
        
        plugin_manager_->set_error_callback([this](const std::string& plugin_name, 
                                                  const std::string& error) {
            LOG_ERROR("Plugin error: {} - {}", plugin_name, error);
        });
        
        // 5. Подписка на сигналы
        packet_signal_->connect([this](const header_t* header,
                                      const endpoint_t* endpoint,
                                      std::span<const char> payload) {
            this->on_packet_received(header, endpoint, payload);
        });
        
        conn_state_signal_->connect([this](const char* uri, conn_state_t state) {
            this->on_connection_state_changed(uri, state);
        });
        
        // 6. Загрузка плагинов если включено
        if (config.get_or<bool>("plugins.auto_load", true)) {
            LOG_INFO("Auto-loading plugins...");
            plugin_manager_->load_all_plugins();
        }
        LOG_INFO("Core initialized successfully");
        
    } catch (const std::exception& e) {
        LOG_CRITICAL("Core initialization failed: {}", e.what());
        cleanup();
        throw;
    }
}

Core::~Core() {
    LOG_INFO("Core shutting down...");
    
    stop(); // Останавливаем если работает
    
    // Уничтожаем компоненты в правильном порядке
    plugin_manager_.reset();
    conn_state_signal_.reset();
    packet_signal_.reset();
    host_api_.reset();
    
    // Останавливаем потоки
    if (!io_threads_.empty()) {
        work_guard_.reset();
        io_context_.stop();
        
        for (auto& thread : io_threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        io_threads_.clear();
    }
    
    instance_ = nullptr;
    LOG_INFO("Core shutdown complete");
}

bool Core::start() {
    if (!Stats::is_initialized) {
        LOG_ERROR("Cannot start: Core not initialized");
        return false;
    }
    
    if (Stats::is_running) {
        LOG_WARN("Core already running");
        return true;
    }
    
    try {
        LOG_INFO("Starting Core...");
        
        // Запускаем потоки IO
        initialize_io_threads();
        
        // Запускаем компоненты
        initialize_components();
        
        Stats::is_running = true;
        LOG_INFO("Core started successfully");
        return true;
        
    } catch (const std::exception& e) {
        LOG_CRITICAL("Failed to start Core: {}", e.what());
        Stats::is_running = false;
        return false;
    }
}

void Core::stop() {
    if (!Stats::is_running) {
        return;
    }
    
    LOG_INFO("Stopping Core...");
    Stats::is_running = false;
    
    // Останавливаем компоненты
    cleanup();
    
    LOG_INFO("Core stopped");
}

void Core::initialize_components() {
    // TODO: Инициализация ConnectManager и HomeServices
    LOG_INFO("Components initialized");
}

void Core::initialize_io_threads() {
    if (!io_threads_.empty()) {
        LOG_WARN("IO threads already initialized");
        return;
    }
    
    LOG_INFO("Starting {} IO threads...", io_thread_count_);
    
    for (unsigned int i = 0; i < io_thread_count_; ++i) {
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
    
    LOG_INFO("IO threads started");
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

void Core::on_packet_received(const header_t* header,
                             const endpoint_t* endpoint,
                             std::span<const char> payload) {
    Stats::packets_received++;
    
    // Проверка магического числа
    if (header->magic != GNET_MAGIC) {
        LOG_ERROR("Invalid packet magic: 0x{:08X}", header->magic);
        return;
    }
    
    // TODO: Обработка через плагины-обработчики
    LOG_DEBUG("Packet received: type={}, size={}", 
                 header->payload_type, payload.size());
}

void Core::on_connection_state_changed(const char* uri, conn_state_t state) {
    LOG_INFO("Connection state changed: {} -> {}", 
                uri ? uri : "unknown", 
                static_cast<int>(state));
}

} // namespace gn
