# 12 — C++ SDK

`sdk/cpp/handler.hpp` · `sdk/cpp/connector.hpp` · `sdk/cpp/data.hpp` · `sdk/cpp/plugin.hpp`

---

## IHandler

```cpp
class ChatHandler : public gn::IHandler {
public:
    const char* get_plugin_name() const override { return "chat"; }

    // Возвращает plugin_info_t: имя, версия, приоритет, capability flags.
    const plugin_info_t* get_plugin_info() const override {
        static plugin_info_t info{ "chat", 0x00010000, 200, {}, 0 };
        return &info;  // priority=200: этот хендлер вызывается раньше дефолтных
    }

    void on_init() override {
        set_supported_types({MSG_TYPE_CHAT});
        // Пустой список или не вызывать → wildcard (все типы)
    }

    // Основной коллбэк: пакет из ESTABLISHED соединения, расшифрован.
    // endpoint->peer_id == conn_id для прямого ответа.
    void handle_message(const header_t*   hdr,
                         const endpoint_t* ep,
                         const void*       payload,
                         size_t            size) override
    {
        LOG_INFO("Chat from {}:{} type={}", ep->address, ep->port,
                 hdr->payload_type);

        // Прямой ответ без поиска URI:
        std::string reply = "pong";
        send_response(ep->peer_id, MSG_TYPE_CHAT,
                      reply.data(), reply.size());

        // Или отправить другому пиру по URI:
        send("tcp://10.0.0.2:25565", MSG_TYPE_CHAT,
             payload, size);

        // Найти соединение по публичному ключу пира:
        conn_id_t peer_conn = find_conn("a3f5c8d2...64hex...");
        if (peer_conn != CONN_ID_INVALID)
            send_response(peer_conn, MSG_TYPE_CHAT, payload, size);
    }

    // Цепочка ответственности. Вызывается ПОСЛЕ handle_message().
    // Управляет передачей пакета следующему хендлеру.
    propagation_t on_result(const header_t* /*hdr*/,
                             uint32_t /*msg_type*/) override {
        return PROPAGATION_CONSUMED;  // обработали — стоп
        // PROPAGATION_CONTINUE → следующий хендлер по приоритету
        // PROPAGATION_REJECT   → дропнуть пакет
    }

    void on_connection_state(const char* uri, conn_state_t state) override {
        if (state == STATE_ESTABLISHED)
            LOG_INFO("Peer connected: {}", uri);
        else if (state == STATE_CLOSED)
            LOG_INFO("Peer disconnected: {}", uri);
    }

    void on_shutdown() override { /* cleanup */ }
};

HANDLER_PLUGIN(ChatHandler)
```

### Вспомогательные методы IHandler

```cpp
// protected — доступны в derived классах:

// Отправить пиру по URI (подключается если ещё нет соединения).
void send(const char* uri, uint32_t type, const void* data, size_t size);

// Ответить на существующее соединение — используйте endpoint->peer_id.
void send_response(conn_id_t conn_id, uint32_t type,
                   const void* data, size_t size);

// Найти conn_id для ESTABLISHED пира по hex-encoded pubkey (64 символа).
// Возвращает CONN_ID_INVALID если не найден.
conn_id_t find_conn(const char* pubkey_hex_64) const;

// Подписать данные device Ed25519 ключом ядра. Возвращает 0 на успех.
int sign(const void* data, size_t size, uint8_t sig[64]);

// Верифицировать Ed25519 подпись. Возвращает 0 если валидна.
int verify(const void* data, size_t size,
           const uint8_t* pubkey, const uint8_t* sig);

// Логировать через логгер ядра (level: 0=trace..5=critical).
void log(int level, const char* file, int line, const char* msg);

// Подписать на конкретные типы сообщений (вызывать из on_init()).
void set_supported_types(std::initializer_list<uint32_t> types);
```

---

## Цепочка ответственности: on_result()

Несколько хендлеров могут быть подписаны на один тип сообщений. Они вызываются в порядке убывания `priority` из `plugin_info_t`. `on_result()` управляет передачей управления:

```
Порядок вызова для MSG_TYPE_CHAT:

  security_handler (priority=255) → handle_message → on_result → CONTINUE
        ↓
  chat_handler     (priority=200) → handle_message → on_result → CONSUMED ← стоп
  logger_handler   (priority=  0) → ← не вызывается
```

```cpp
// Пример: security handler пропускает легитимные пакеты
propagation_t on_result(const header_t*, uint32_t) override {
    if (last_packet_ok_)
        return PROPAGATION_CONTINUE; // передать дальше
    return PROPAGATION_REJECT;       // заблокировать
}
```

---

## IConnector

```cpp
class MyConnector : public gn::IConnector {
public:
    std::string get_scheme() const override { return "tcp"; }
    std::string get_name()   const override { return "GoodNet TCP"; }

    const plugin_info_t* get_plugin_info() const override {
        static plugin_info_t info{ "tcp_connector", 0x00010000, 128, {}, 0 };
        return &info;
    }

    void on_init() override {
        // io_context, acceptor, thread pool

        // Зарегистрировать дополнительный хендлер (например ICE signaling):
        // signal_handler_.name = "ice_signal";
        // register_extra_handler(&signal_handler_);
    }

    int do_connect(const char* uri) override {
        endpoint_t ep{}; /* заполнить из uri */
        conn_id_t id = notify_connect(&ep); // → ядро зарегистрировало
        // сохранить id рядом с сокетом
        return 0;
    }

    int do_listen(const char* host, uint16_t port) override {
        // acceptor loop; при accept: notify_connect(&ep) → conn_id
        return 0;
    }

    int do_send_to(conn_id_t id, const void* data, size_t size) override {
        // найти сокет по id, async_write
        return 0;
    }

    void do_close(conn_id_t id) override {
        // закрыть сокет
        notify_disconnect(id, 0); // ядро узнало об отключении
    }

    void on_shutdown() override { /* stop io_context, join threads */ }
};

CONNECTOR_PLUGIN(MyConnector)
```

### Вспомогательные методы IConnector

```cpp
// protected:

// Уведомить ядро о новом соединении. Возвращает conn_id.
conn_id_t notify_connect(const endpoint_t* ep);

// Передать сырые байты ядру для reassembly и dispatch.
void notify_data(conn_id_t id, const void* data, size_t size);

// Уведомить ядро о закрытии соединения.
void notify_disconnect(conn_id_t id, int error = 0);

// Найти conn_id ESTABLISHED пира по hex pubkey.
conn_id_t find_peer_conn(const char* pubkey_hex_64) const;

// Зарегистрировать дополнительный handler_t (ICE back-channel).
// Вызывать только из on_init()!
void register_extra_handler(handler_t* h);

// Логировать через ядро.
void log(int level, const char* file, int line, const char* msg);
```

---

## PodData\<T\>

Нулекопирующая обёртка для POD-структур payload.

```cpp
#pragma pack(push, 1)
struct HeartbeatPayload {
    uint64_t timestamp_us;
    uint32_t seq;
    uint8_t  flags;  // 0x00=ping, 0x01=pong
    uint8_t  _pad[3];
};
#pragma pack(pop)

using HeartbeatMsg = gn::sdk::PodData<HeartbeatPayload>;

// Отправка:
HeartbeatMsg ping;
ping->seq   = ++counter_;
ping->flags = 0x00;
auto wire   = ping.serialize();   // RawBuffer = vector<uint8_t>
send_response(ep->peer_id, MSG_TYPE_HEARTBEAT, wire.data(), wire.size());

// Приём:
auto pong = gn::sdk::from_bytes<HeartbeatMsg>(payload, size);
LOG_DEBUG("Pong seq={} ts={}", pong->seq, pong->timestamp_us);
```

---

## Макросы HANDLER_PLUGIN / CONNECTOR_PLUGIN

```cpp
#define HANDLER_PLUGIN(ClassName)                                               \
    static ClassName _gn_plugin_instance;                                       \
                                                                                \
    extern "C" GN_EXPORT                                                        \
    const plugin_info_t* plugin_get_info() {                                    \
        return _gn_plugin_instance.get_plugin_info();                           \
    }                                                                           \
                                                                                \
    extern "C" GN_EXPORT                                                        \
    int handler_init(host_api_t* api, handler_t** out) {                        \
        _gn_plugin_instance.init(api);                                          \
        *out = _gn_plugin_instance.to_c_handler();                              \
        return 0;                                                               \
    }

#define CONNECTOR_PLUGIN(ClassName)                                             \
    static ClassName _gn_connector_instance;                                    \
                                                                                \
    extern "C" GN_EXPORT                                                        \
    const plugin_info_t* plugin_get_info() {                                    \
        return _gn_connector_instance.get_plugin_info();                        \
    }                                                                           \
                                                                                \
    extern "C" GN_EXPORT                                                        \
    int connector_init(host_api_t* api, connector_ops_t** out) {                \
        _gn_connector_instance.init(api);                                       \
        *out = _gn_connector_instance.to_c_ops();                               \
        return 0;                                                               \
    }
```

- `static ClassName instance` — один экземпляр на `.so` (Meyers singleton)
- `plugin_get_info()` — вызывается PluginManager **до** `*_init()` для чтения метаданных
- `GN_EXPORT` — `__attribute__((visibility("default")))` или `__declspec(dllexport)`

---

*← [11 — C SDK](11-sdk-c.md) · [13 — Сборка →](13-build.md)*