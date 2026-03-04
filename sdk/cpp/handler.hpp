#pragma once

#include "../handler.h"

#include <vector>
#include <string>
#include <cstring>

namespace gn {

// ─── IHandler ────────────────────────────────────────────────────────────────
//
// Базовый C++ класс для хендлеров.
// Плагин наследует IHandler, переопределяет виртуальные методы,
// добавляет в конец файла HANDLER_PLUGIN(MyHandlerClass).
//
// Ядро вызывает handle_message() для каждого пакета из ESTABLISHED соединений
// с типом из supported_types (установленных в on_init()).

class IHandler {
protected:
    handler_t             handler_;
    std::vector<uint32_t> supported_types_;
    host_api_t*           api_ = nullptr;

public:
    IHandler() {
        std::memset(&handler_, 0, sizeof(handler_));
        handler_.user_data = this;
    }
    virtual ~IHandler() = default;

    // Вызывается из HANDLER_PLUGIN макроса — не переопределять
    void init(host_api_t* api) {
        api_ = api;
        on_init();
    }

    // Заполнить handler_ C-коллбэками и вернуть указатель
    handler_t* to_c_handler() {
        // name
        handler_.name = get_plugin_name();

        // handle_message
        handler_.handle_message = [](void* ud,
                                     const header_t* h,
                                     const endpoint_t* ep,
                                     const void* pl, size_t sz) {
            static_cast<IHandler*>(ud)->handle_message(h, ep, pl, sz);
        };

        // handle_conn_state
        handler_.handle_conn_state = [](void* ud,
                                        const char* uri,
                                        conn_state_t st) {
            static_cast<IHandler*>(ud)->handle_connection_state(uri, st);
        };

        // shutdown
        handler_.shutdown = [](void* ud) {
            static_cast<IHandler*>(ud)->on_shutdown();
        };

        handler_.supported_types     = supported_types_.data();
        handler_.num_supported_types = supported_types_.size();

        return &handler_;
    }

    // ── Для наследника ─────────────────────────────────────────────────────────

    // Уникальное имя плагина — ключ для find_handler_by_name()
    virtual const char* get_plugin_name() const = 0;

    // Вызывается один раз при загрузке — задать supported_types, инициализировать ресурсы
    virtual void on_init() {}

    // Основной коллбэк — получен пакет из ESTABLISHED соединения
    virtual void handle_message(const header_t*   header,
                                 const endpoint_t* endpoint,
                                 const void*       payload,
                                 size_t            payload_size) = 0;

    // Изменилось состояние соединения (опционально)
    virtual void handle_connection_state(const char* /*uri*/,
                                          conn_state_t /*state*/) {}

    // Вызывается перед dlclose() — освободить ресурсы
    virtual void on_shutdown() {}

protected:
    void set_supported_types(std::initializer_list<uint32_t> types) {
        supported_types_.assign(types);
        handler_.supported_types     = supported_types_.data();
        handler_.num_supported_types = supported_types_.size();
    }

    // Отправить пакет через ConnectionManager
    void send(const char* uri, uint32_t type,
              const void* data, size_t size) {
        if (api_ && api_->send)
            api_->send(api_->ctx, uri, type, data, size);
    }

    // Подписать буфер device_seckey'ом ядра
    int sign(const void* data, size_t size, uint8_t sig[64]) {
        if (!api_ || !api_->sign_with_device) return -1;
        return api_->sign_with_device(api_->ctx, data, size, sig);
    }

    // Проверить подпись
    int verify(const void* data, size_t size,
               const uint8_t* pubkey, const uint8_t* sig) {
        if (!api_ || !api_->verify_signature) return -1;
        return api_->verify_signature(api_->ctx, data, size, pubkey, sig);
    }
};

} // namespace gn
