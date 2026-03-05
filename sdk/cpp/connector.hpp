#pragma once

#include "../connector.h"

#include <string>
#include <cstring>
#include <span>

namespace gn {

namespace sdk {
    class IConnection {
    public:
        virtual ~IConnection() = default;

        // Вызывается при установке соединения
        virtual void on_connect(const endpoint_t* remote) = 0;

        // Новые данные пришли (сырые байты)
        virtual void on_data(std::span<const uint8_t> data) = 0;

        // Соединение закрыто
        virtual void on_disconnect(int error_code) = 0;

        // Отправить данные (реализация коннектора сама решает как)
        virtual void send(std::span<const uint8_t> data) = 0;

        // Опционально: имя схемы (tcp, ws, bluetooth...)
        virtual std::string scheme() const = 0;
    };
}

// ─── IConnector ──────────────────────────────────────────────────────────────
//
// Базовый класс для транспортных плагинов.
//
// Плагин наследует IConnector, реализует виртуальные методы,
// добавляет CONNECTOR_PLUGIN(MyConnectorClass) в конец файла.
//
// Важно: коннектор вызывает api_->on_connect() / on_data() / on_disconnect()
// чтобы уведомлять ядро о событиях соединений.
// conn_id_t который возвращает on_connect() нужно хранить рядом с сокетом.

class IConnector {
protected:
    connector_ops_t ops_{};
    host_api_t*     api_ = nullptr;

public:
    IConnector() {
        ops_.connector_ctx = this;
    }
    virtual ~IConnector() = default;

    // Вызывается из CONNECTOR_PLUGIN макроса
    void init(host_api_t* api) {
        api_ = api;
        on_init();
    }

    connector_ops_t* to_c_ops() {
        ops_.connect = [](void* ctx, const char* uri) -> int {
            return static_cast<IConnector*>(ctx)->do_connect(uri);
        };
        ops_.listen = [](void* ctx, const char* host, uint16_t port) -> int {
            return static_cast<IConnector*>(ctx)->do_listen(host, port);
        };
        ops_.send_to = [](void* ctx, conn_id_t id,
                          const void* data, size_t size) -> int {
            return static_cast<IConnector*>(ctx)->do_send_to(id, data, size);
        };
        ops_.close = [](void* ctx, conn_id_t id) {
            static_cast<IConnector*>(ctx)->do_close(id);
        };
        ops_.get_scheme = [](void* ctx, char* buf, size_t sz) {
            auto s = static_cast<IConnector*>(ctx)->get_scheme();
            std::strncpy(buf, s.c_str(), sz - 1);
            buf[sz - 1] = '\0';
        };
        ops_.get_name = [](void* ctx, char* buf, size_t sz) {
            auto n = static_cast<IConnector*>(ctx)->get_name();
            std::strncpy(buf, n.c_str(), sz - 1);
            buf[sz - 1] = '\0';
        };
        ops_.shutdown = [](void* ctx) {
            static_cast<IConnector*>(ctx)->on_shutdown();
        };
        return &ops_;
    }

    // ── Для наследника ─────────────────────────────────────────────────────────

    virtual void on_init()     {}
    virtual void on_shutdown() {}

    // Схема URI: "tcp", "mock", …
    virtual std::string get_scheme() const = 0;
    virtual std::string get_name()   const = 0;

    // Инициировать подключение к uri. Результат — через api_->on_connect().
    virtual int  do_connect(const char* uri)                        = 0;

    // Начать слушать. Входящие → api_->on_connect().
    virtual int  do_listen(const char* host, uint16_t port)         = 0;

    // Отправить bytes в соединение conn_id.
    virtual int  do_send_to(conn_id_t id,
                             const void* data, size_t size)          = 0;

    // Закрыть соединение conn_id. Плагин вызовет api_->on_disconnect().
    virtual void do_close(conn_id_t id)                              = 0;

protected:
    // Сообщить ядру о новом соединении → получить conn_id
    conn_id_t notify_connect(const endpoint_t* ep) {
        if (api_ && api_->on_connect)
            return api_->on_connect(api_->ctx, ep);
        return CONN_ID_INVALID;
    }

    // Передать ядру сырые байты
    void notify_data(conn_id_t id, const void* data, size_t size) {
        if (api_ && api_->on_data)
            api_->on_data(api_->ctx, id, data, size);
    }

    // Сообщить ядру об отключении
    void notify_disconnect(conn_id_t id, int error = 0) {
        if (api_ && api_->on_disconnect)
            api_->on_disconnect(api_->ctx, id, error);
    }
};

} // namespace gn
