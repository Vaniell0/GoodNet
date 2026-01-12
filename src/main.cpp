#include "config.hpp"
#include "logger.hpp"
#include <iostream>
#include <vector>
#include <map>

void test_function() {
    SCOPED_TRACE();  // Автоматически логирует вход/выход
    
    int x = 42;
    std::string name = "GoodNet";
    std::vector<int> vec = {1, 2, 3, 4, 5};
    std::map<std::string, int> config = {{"port", 8080}, {"timeout", 30}};
    
    // Логирование значений переменных
    TRACE_VALUE(x);
    DEBUG_VALUE(name);
    INFO_VALUE(x + 10);
    
    // Подробное логирование
    TRACE_VALUE_DETAILED(x);
    
    // Логирование указателей
    int* ptr = &x;
    TRACE_POINTER(ptr);
    
    LOG_DEBUG("Debug build specific logging");
}

int main(int argc, char* argv[]) {
    try {        
        Config config;

        Logger::initialize(
            config.get_or<std::string>("logging.level", "info"),
            config.get_or<std::string>("logging.file", "logs/goodnet.log"),
            config.get_or<int>("logging.max_size", 10 * 1024 * 1024),
            config.get_or<int>("logging.max_files", 5)
        );
        
        LOG_INFO("GoodNet starting...");
        LOG_INFO("Listen address: {}", config.get_or<std::string>("core.listen_address", "0.0.0.0"));
        LOG_INFO("Listen port: {}", config.get_or<int>("core.listen_port", 25565));
        
        // Демонстрация разных уровней логирования
        LOG_TRACE("This is trace message");
        LOG_DEBUG("This is debug message");
        LOG_INFO("This is info message");
        LOG_WARN("This is warning message");
        LOG_ERROR("This is error message");
        
        test_function();
        
        Logger::shutdown();
        return 0;

    } catch (const std::exception& e) {
        LOG_CRITICAL("Fatal error: {}", e.what());
        return 1;
    }
}