#include "../sdk/cpp/handler.hpp"
#include "../sdk/cpp/plugin.hpp"
#include <iostream>
#include <cstring>

class LoggerHandler : public gn::IHandler {
public:
    LoggerHandler() {
        // Устанавливаем поддерживаемые типы
        std::vector<uint32_t> types = {MSG_TYPE_SYSTEM};
        set_supported_types(types);
    }
    
    void on_init() override {
        log("Logger handler initialized");
    }
    
    void handle_message(
        const header_t* header,
        const endpoint_t* endpoint,
        const void* payload,
        size_t payload_size
    ) override {
        // Простое логирование
        std::string message = "[Logger] Received message type: " + 
                             std::to_string(header->payload_type) +
                             " from: " + endpoint->address + 
                             ":" + std::to_string(endpoint->port);
        log(message.c_str());
    }
};

HANDLER_PLUGIN(LoggerHandler)
