# 12 — C++ SDK

`sdk/cpp/handler.hpp` · `sdk/cpp/connector.hpp` · `sdk/cpp/data.hpp` · `sdk/cpp/plugin.hpp`

---

## IHandler

```cpp
class MyHandler : public gn::IHandler {
public:
    // Обязательно — ключ для find_handler_by_name()
    const char* get_plugin_name() const override { return "my_handler"; }

    // Вызывается один раз при загрузке
    void on_init() override {
        set_supported_types({MSG_TYPE_CHAT, MSG_TYPE_FILE});
        // Пустой список = wildcard (все типы)
    }

    // Основной коллбэк — пакет из ESTABLISHED соединения
    void handle_message(const header_t*   hdr,
                        const endpoint_t* ep,
                        const void*       payload,
                        size_t            size) override
    {
        LOG_INFO("Got msg type={} from {}:{}", hdr->payload_type, ep->address, ep->port);

        // Ответить:
        std::string reply = "pong";
        send(ep->address, MSG_TYPE_CHAT, reply.data(), reply.size());

        // Подписать данные device_key'ом ядра:
        uint8_t sig[64];
        sign(payload, size, sig);
    }

    // Опционально — изменение состояния соединения
    void handle_connection_state(const char* uri, conn_state_t state) override {
        if (state == STATE_ESTABLISHED)
            LOG_INFO("Peer connected: {}", uri);
    }

    // Вызывается перед dlclose()
    void on_shutdown() override { /* cleanup */ }
};

HANDLER_PLUGIN(MyHandler)  // ← в конце .cpp
```

### Вспомогательные методы IHandler

```cpp
// В protected секции IHandler:
void send(const char* uri, uint32_t type, const void* data, size_t size);
int  sign  (const void* data, size_t size, uint8_t sig[64]);
int  verify(const void* data, size_t size, const uint8_t* pk, const uint8_t* sig);
void set_supported_types(std::initializer_list<uint32_t> types);
```

---

## IConnector

```cpp
class MyConnector : public gn::IConnector {
public:
    std::string get_scheme() const override { return "tcp"; }
    std::string get_name()   const override { return "TCP Connector"; }

    void on_init() override {
        // setup io_context, acceptor, thread pool
    }

    // Начать async подключение к uri
    int do_connect(const char* uri) override {
        // resolve + async_connect ...
        // При успехе:
        endpoint_t ep{}; strncpy(ep.address, host, sizeof(ep.address)); ep.port = port;
        conn_id_t id = notify_connect(&ep);  // → ядро зарегистрировало соединение
        // сохранить id рядом с сокетом
        return 0;
    }

    int do_listen(const char* host, uint16_t port) override {
        // setup acceptor, начать accept loop
        // при каждом accept: notify_connect(&ep) → conn_id
        return 0;
    }

    int do_send_to(conn_id_t id, const void* data, size_t size) override {
        // найти сокет по id, async_write
        return 0;
    }

    void do_close(conn_id_t id) override {
        // закрыть сокет
        notify_disconnect(id, 0);  // → ядро узнало об отключении
    }

    void on_shutdown() override {
        // остановить io_context, join threads
    }
};

CONNECTOR_PLUGIN(MyConnector)  // ← в конце .cpp
```

### Вспомогательные методы IConnector

```cpp
// В protected секции IConnector:
conn_id_t notify_connect   (const endpoint_t* ep);
void      notify_data      (conn_id_t id, const void* data, size_t size);
void      notify_disconnect(conn_id_t id, int error = 0);
```

---

## PodData\<T\>

Нулекопирующая обёртка для POD-структур payload.

```cpp
// Определение типа:
#pragma pack(push, 1)
struct HeartbeatPayload {
    uint32_t seq;
    uint8_t  flags;  // 0x00=ping, 0x01=pong
};
#pragma pack(pop)
using HeartbeatMsg = gn::sdk::PodData<HeartbeatPayload>;

// Отправка:
HeartbeatMsg ping;
ping->seq   = ++counter;
ping->flags = 0x00;
auto wire = ping.serialize();  // RawBuffer = vector<uint8_t>
send(uri, MSG_TYPE_HEARTBEAT, wire.data(), wire.size());

// Приём:
auto pong = gn::sdk::from_bytes<HeartbeatMsg>(payload, size);
LOG_DEBUG("Pong seq={}", pong->seq);
```

---

## Макросы HANDLER_PLUGIN / CONNECTOR_PLUGIN

```cpp
// sdk/cpp/plugin.hpp:

#define HANDLER_PLUGIN(ClassName)                                             \
extern "C" GN_EXPORT int handler_init(host_api_t* api, handler_t** out) {    \
    static ClassName instance;                                                \
    if (!api || api->plugin_type != PLUGIN_TYPE_HANDLER) return -1;          \
    instance.init(api);                                                       \
    *out = instance.to_c_handler();                                           \
    return 0;                                                                 \
}

#define CONNECTOR_PLUGIN(ClassName)                                               \
extern "C" GN_EXPORT int connector_init(host_api_t* api, connector_ops_t** out) { \
    static ClassName instance;                                                    \
    if (!api || api->plugin_type != PLUGIN_TYPE_CONNECTOR) return -1;            \
    instance.init(api);                                                           \
    *out = instance.to_c_ops();                                                   \
    return 0;                                                                     \
}
```

- `static ClassName instance` — один экземпляр на `.so`, гарантия Meyers singleton
- `GN_EXPORT` — `__attribute__((visibility("default")))` или `__declspec(dllexport)`
- Проверка `plugin_type` предотвращает загрузку Handler как Connector (ошибка конфига)

---

*← [11 — C SDK](11-sdk-c.md) · [13 — Сборка →](13-build.md)*
