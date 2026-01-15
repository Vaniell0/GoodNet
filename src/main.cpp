#include "config.hpp"
#include "logger.hpp"
#include "core.hpp"
#include <csignal>
#include <atomic>

namespace {
    std::atomic<bool> g_running{true};
    
    void signal_handler(int signal) {
        const char* signal_name = "";
        switch (signal) {
            case SIGINT: signal_name = "SIGINT"; break;
            case SIGTERM: signal_name = "SIGTERM"; break;
            case SIGHUP: signal_name = "SIGHUP"; break;
            default: signal_name = "UNKNOWN"; break;
        }
        LOG_INFO("Signal {} ({}) received, shutting down...", signal_name, signal);
        g_running = false;
    }
}

int main(int argc, char* argv[]) {
    try {
        // 1. Инициализация конфигурации
        Config config;
        
        // 2. Инициализация логгера
        Logger::initialize(
            config.get_or<std::string>("logging.level", "info"),
            config.get_or<std::string>("logging.file", "logs/goodnet.log"),
            config.get_or<int>("logging.max_size", 10 * 1024 * 1024),
            config.get_or<int>("logging.max_files", 5)
        );
        
        LOG_INFO("\n┌──────────────────────────────────────────────┐\n"
                   "│              GoodNet v0.1.0                  │\n"
                   "│       Advanced Network Framework             │\n"
                   "└──────────────────────────────────────────────┘");
        
        LOG_INFO("Listen address: {}", config.get_or<std::string>("core.listen_address", "0.0.0.0"));
        LOG_INFO("Listen port: {}", config.get_or<int>("core.listen_port", 25565));
        LOG_INFO("IO threads: {}", config.get_or<int>("core.io_threads", 4));
        
        // 3. Настройка обработки сигналов
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);
        
        // 4. Создание и запуск ядра
        auto core = std::make_unique<gn::Core>(config);
        
        if (!core->start()) {
            LOG_CRITICAL("Failed to start Core");
            return 1;
        }
        
        LOG_INFO("GoodNet started successfully. Press Ctrl+C to stop.");
        
        // 5. Главный цикл
        while (g_running && core->is_running()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            
            
        }
        
        // 6. Корректное завершение
        LOG_INFO("Shutting down GoodNet...");
        core->stop();
        
        Logger::shutdown();
        
        LOG_INFO("GoodNet shutdown complete");
        return 0;
        
    } catch (const std::exception& e) {
        LOG_CRITICAL("Fatal error: {}", e.what());
        return 1;
    } catch (...) {
        LOG_CRITICAL("Fatal unknown error");
        return 1;
    }
}