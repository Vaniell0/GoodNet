#pragma once

#include "handler.h"

#include <stdexcept>
#include <vector>
#include <string>
#include <functional>

namespace gn {

class IHandler {
protected:
    handler_t handler_;
    std::vector<uint32_t> supported_types_;
    
    host_api_t* api_ = nullptr;
    
public:
    IHandler() {
        /* ИНИЦИАЛИЗАЦИЯ НУЛЯМИ */
        handler_.handle_message = nullptr;
        handler_.handle_conn_state = nullptr;
        handler_.shutdown = nullptr;  // <-- Инициализируем
        handler_.supported_types = nullptr;
        handler_.num_supported_types = 0;
        handler_.user_data = this;
    }
    virtual ~IHandler() = default;
    
    void init(host_api_t* api) {
        if (api && api->api_version != GNET_API_VERSION) {
            // Можно выбросить исключение или как-то обработать
            throw std::runtime_error("API version mismatch");
        }
        api_ = api;
        on_init();
    }
    
    void set_supported_types(const std::vector<uint32_t>& types) {
        supported_types_ = types;
        handler_.supported_types = supported_types_.data();
        handler_.num_supported_types = supported_types_.size();
    }
    
    handler_t* to_c_handler() {
        handler_.handle_message = [](void* user_data,
                                     const header_t* header,
                                     const endpoint_t* endpoint,
                                     const void* payload,
                                     size_t payload_size) {
            IHandler* self = static_cast<IHandler*>(user_data);
            self->handle_message(header, endpoint, payload, payload_size);
        };
        
        handler_.handle_conn_state = [](void* user_data,
                                        const char* uri,
                                        conn_state_t state) {
            IHandler* self = static_cast<IHandler*>(user_data);
            self->handle_connection_state(uri, state);
        };

        // Устанавливаем функцию shutdown
        handler_.shutdown = [](void* user_data) {
            IHandler* self = static_cast<IHandler*>(user_data);
            self->shutdown();
        };
        
        return &handler_;
    }
    
    virtual void on_init() {}
    
    virtual void handle_message(
        const header_t* header,
        const endpoint_t* endpoint,
        const void* payload,
        size_t payload_size
    ) = 0;
    
    virtual void handle_connection_state(
        const char* uri,
        conn_state_t state
    ) {}

    // Добавляем виртуальную функцию shutdown
    virtual void shutdown() {}
    
protected:   
    void send(const char* uri, uint32_t type, const void* data, size_t size) {
        if (api_ && api_->send) {
            api_->send(uri, type, data, size);
        }
    }
};

}  /* namespace gn */