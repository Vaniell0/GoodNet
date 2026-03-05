#pragma once
#include "plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

// ─── connector_ops_t ─────────────────────────────────────────────────────────
//
// C-интерфейс коннектора.
// Коннектор — транспортный плагин (TCP, UDP, WebSocket, …).
//
// Модель владения соединениями:
//   Плагин ВЛАДЕЕТ объектами соединений (IConnection, boost::tcp::socket, …).
//   Ядро (ConnectionManager) ВЛАДЕЕТ только записью ConnectionRecord,
//   идентифицируемой conn_id_t.
//
//   conn_id_t — непрозрачный ключ, который ядро передаёт плагину через
//   on_connect() коллбэк (в host_api_t). Плагин хранит его рядом с сокетом.
//
// Жизненный цикл соединения:
//   1. Входящее (listen): accept → api->on_connect() → conn_id → хранить в сокете
//   2. Исходящее (connect): подключиться → api->on_connect() → conn_id → хранить
//   3. Данные: прочитать → api->on_data(conn_id, buf, len)
//   4. Закрытие: api->on_disconnect(conn_id, error)
//   5. send_to: ядро говорит коннектору отправить bytes через conn_id

typedef struct connector_ops_t {

    // ── Управление соединениями ───────────────────────────────────────────────

    // Подключиться к uri (например "tcp://192.168.1.1:25565").
    // SYNC: функция должна вернуться быстро; подключение — в фоне.
    // После установки: вызвать api->on_connect() → получить conn_id.
    // Возвращает 0 если запрос принят, -1 при ошибке.
    int (*connect)(void* connector_ctx, const char* uri);

    // Начать слушать на адресе/порту.
    // При каждом входящем соединении вызывать api->on_connect().
    // Возвращает 0 при успехе.
    int (*listen)(void* connector_ctx, const char* host, uint16_t port);

    // Отправить сырые байты в соединение с данным conn_id.
    // Плагин находит сокет по conn_id и записывает bytes.
    // Возвращает 0 при успехе, -1 при ошибке.
    int (*send_to)(void* connector_ctx,
                   conn_id_t conn_id,
                   const void* data, size_t size);

    // Закрыть соединение conn_id.
    // Плагин закрывает сокет. Ядро получит on_disconnect().
    void (*close)(void* connector_ctx, conn_id_t conn_id);

    // ── Идентификация ─────────────────────────────────────────────────────────

    // Схема URI: "tcp", "udp", "ws", "mock", …
    // PluginManager использует это для find_connector_by_scheme().
    void (*get_scheme)(void* connector_ctx, char* buf, size_t buf_size);

    // Человекочитаемое имя: "TCP Connector", "MockConnector", …
    void (*get_name)(void* connector_ctx, char* buf, size_t buf_size);

    // ── Жизненный цикл ────────────────────────────────────────────────────────

    // Вызывается ядром перед dlclose(). Закрыть все соединения и освободить ресурсы.
    void (*shutdown)(void* connector_ctx);

    // Непрозрачный контекст плагина (обычно this).
    void* connector_ctx;

} connector_ops_t;

// Сигнатура функции инициализации (CONNECTOR_PLUGIN генерирует автоматически)
typedef int (*connector_init_t)(host_api_t* api, connector_ops_t** out_ops);

#ifdef __cplusplus
}
#endif
