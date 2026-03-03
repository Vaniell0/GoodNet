#pragma once

#include "handler.h"

#include <vector>
#include <string>

namespace gn {

// Базовый класс для обработчиков на C++.
// Наследуй, переопредели handle_message, зарегистрируй через HANDLER_PLUGIN(MyClass).
class IHandler {
protected:
    handler_t             handler_{};
    std::vector<uint32_t> supported_types_;
    host_api_t*           api_ = nullptr;

public:
    IHandler() {
        handler_.api_version         = EXPECTED_API_VERSION;
        handler_.name                = nullptr;   // заполняется в init() из get_plugin_name()
        handler_.shutdown            = nullptr;
        handler_.handle_message      = nullptr;
        handler_.handle_conn_state   = nullptr;
        handler_.supported_types     = nullptr;
        handler_.num_supported_types = 0;
        handler_.user_data           = this;
    }

    virtual ~IHandler() = default;

    // Вызывается из точки входа плагина (get_handler)
    void init(host_api_t* api) {
        api_          = api;
        handler_.name = get_plugin_name();
        on_init();
    }

    void set_supported_types(const std::vector<uint32_t>& types) {
        supported_types_             = types;
        handler_.supported_types     = supported_types_.data();
        handler_.num_supported_types = supported_types_.size();
    }

    // Собирает C-структуру с заполненными указателями на лямбды-диспетчеры
    handler_t* to_c_handler() {
        handler_.handle_message = [](void*            ud,
                                     const header_t*   hdr,
                                     const endpoint_t* ep,
                                     const void*       payload,
                                     size_t            size)
        {
            static_cast<IHandler*>(ud)->handle_message(hdr, ep, payload, size);
        };

        handler_.handle_conn_state = [](void*        ud,
                                        const char*  uri,
                                        conn_state_t state)
        {
            static_cast<IHandler*>(ud)->handle_connection_state(uri, state);
        };

        handler_.shutdown = [](void* ud) {
            static_cast<IHandler*>(ud)->on_shutdown();
        };

        return &handler_;
    }

    // ── Переопределяемые методы ───────────────────────────────────────────────

    // Имя плагина — статическая строка, время жизни >= объекта (обязателен)
    virtual const char* get_plugin_name() const = 0;

    // Вызывается после получения host_api (необязателен)
    virtual void on_init() {}

    // Вызывается перед dlclose (необязателен)
    virtual void on_shutdown() {}

    // Обработка входящего сообщения (обязателен)
    virtual void handle_message(const header_t*   header,
                                const endpoint_t* endpoint,
                                const void*       payload,
                                size_t            payload_size) = 0;

    // Изменение состояния соединения (необязателен)
    virtual void handle_connection_state(const char* /*uri*/,
                                         conn_state_t /*state*/) {}

protected:
    void send(const char* uri, uint32_t type, const void* data, size_t size) const {
        if (api_ && api_->send) api_->send(uri, type, data, size);
    }
};

} // namespace gn
