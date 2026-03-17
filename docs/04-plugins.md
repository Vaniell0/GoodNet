# 04 — Плагины (SDK)

GoodNet использует два типа плагинов: **Handler** (обработка сообщений) и **Connector** (транспорт). Плагины — динамические библиотеки (`.so`), загружаемые через `dlopen(RTLD_NOW | RTLD_LOCAL)` после SHA-256 верификации.

SDK доступен на двух уровнях:
- **C SDK** (`sdk/*.h`) — минимальный ABI, совместим с любым языком через FFI
- **C++ SDK** (`sdk/cpp/*.hpp`) — обёртки с RAII и удобным API

---

## Архитектура плагинов

```
               PluginManager
                   │
         ┌─────────┴──────────┐
         │                    │
    handlers_[name]      connectors_[scheme]
    HandlerInfo {            ConnectorInfo {
      DynLib (.so)             DynLib (.so)
      handler_t*               connector_ops_t*
      host_api_t (копия)       host_api_t (копия)
    }                        }
```

Жизненный цикл загрузки:
1. `exists(path.so)` — файл существует?
2. `verify_metadata()` — SHA-256 хэш совпадает с манифестом `.so.json`
3. `dlopen(RTLD_NOW | RTLD_LOCAL)` — изоляция символов
4. `plugin_get_info()` — метаданные (опционально, до `*_init()`)
5. `handler_init()` или `connector_init()` — инициализация

---

## C SDK

### types.h — базовые типы

```c
#define GNET_MAGIC     0x474E4554U   // 'GNET'
#define GNET_PROTO_VER 2U

typedef uint64_t conn_id_t;
#define CONN_ID_INVALID 0ULL

// Диапазоны типов: 0–99 core | 100–999 built-in | 1000+ user
#define MSG_TYPE_AUTH          1u
#define MSG_TYPE_KEY_EXCHANGE  2u
#define MSG_TYPE_HEARTBEAT     3u
#define MSG_TYPE_RELAY        10u
#define MSG_TYPE_ICE_SIGNAL   11u
#define MSG_TYPE_CHAT        100u
#define MSG_TYPE_FILE        200u
```

### endpoint_t

```c
typedef struct {
    char     address[128];   // IP или hostname
    uint16_t port;
    uint8_t  pubkey[32];     // Ed25519 user pubkey пира (после AUTH)
    uint64_t peer_id;        // conn_id; заполняется ядром при dispatch
} endpoint_t;
```

`peer_id` используйте в `send_response()` для прямого ответа без URI lookup.

### propagation_t

```c
typedef enum {
    PROPAGATION_CONTINUE = 0,  // передать следующему хендлеру
    PROPAGATION_CONSUMED = 1,  // стоп + session affinity
    PROPAGATION_REJECT   = 2   // дропнуть
} propagation_t;
```

### plugin_info_t

```c
typedef struct {
    const char* name;       // уникальное имя
    uint32_t    version;    // (major<<16)|(minor<<8)|patch
    uint8_t     priority;   // 255=первый, 128=дефолт, 0=последний
    uint8_t     _pad[3];
    uint32_t    caps_mask;  // PLUGIN_CAP_*
} plugin_info_t;

#define PLUGIN_CAP_COMPRESS_ZSTD (1U << 0)
#define PLUGIN_CAP_ICE_SUPPORT   (1U << 1)
#define PLUGIN_CAP_HOT_RELOAD    (1U << 2)
```

### plugin_state_t

```c
typedef enum {
    PLUGIN_STATE_PREPARING = 0,  // загружен, не получает трафик
    PLUGIN_STATE_ACTIVE    = 1,  // основной обработчик
    PLUGIN_STATE_DRAINING  = 2,  // обслуживает существующие сессии
    PLUGIN_STATE_ZOMBIE    = 3   // ожидает dlclose
} plugin_state_t;
```

В alpha все плагины сразу `ACTIVE`. Hot-reload (PREPARING → ACTIVE → DRAINING → ZOMBIE) запланирован на beta.

---

## host_api_t (plugin.h)

Единственный канал плагин ↔ ядро. Инъектируется через `*_init()`.

```c
typedef struct host_api_t {
    // Connector → Core
    conn_id_t (*on_connect)(void* ctx, const endpoint_t* ep);
    void (*on_data)(void* ctx, conn_id_t id, const void* raw, size_t size);
    void (*on_disconnect)(void* ctx, conn_id_t id, int error_code);

    // Handler → Core
    void (*send)(void* ctx, const char* uri, uint32_t type,
                 const void* payload, size_t size);
    void (*send_response)(void* ctx, conn_id_t conn_id, uint32_t type,
                          const void* data, size_t len);

    // Crypto
    int (*sign_with_device)(void* ctx, const void* data, size_t size,
                             uint8_t sig[64]);
    int (*verify_signature)(void* ctx, const void* data, size_t size,
                             const uint8_t* pubkey, const uint8_t* signature);

    // Routing
    conn_id_t (*find_conn_by_pubkey)(void* ctx, const char* pubkey_hex_64);
    void (*register_handler)(void* ctx, handler_t* h);

    // Logging
    void (*log)(void* ctx, int level, const char* file, int line,
                const char* msg);

    // Metadata
    void* internal_logger;         // spdlog::logger* для sharing
    plugin_info_t* plugin_info;    // заполняется PluginManager
    plugin_type_t plugin_type;     // deprecated
    void* ctx;                     // передавать первым аргументом
} host_api_t;
```

---

## handler_t (handler.h)

```c
struct handler_t {
    const char* name;

    void (*handle_message)(void* ud, const header_t* hdr,
                           const endpoint_t* ep,
                           const void* payload, size_t size);

    propagation_t (*on_message_result)(void* ud, const header_t* hdr,
                                        uint32_t msg_type);  // NULL → CONTINUE

    void (*handle_conn_state)(void* ud, const char* uri, conn_state_t state);
    void (*shutdown)(void* ud);

    const uint32_t* supported_types;  // NULL/0 → wildcard
    size_t          num_supported_types;
    const plugin_info_t* info;
    void* user_data;
};

typedef int (*handler_init_t)(host_api_t* api, handler_t** out);
typedef const plugin_info_t* (*plugin_get_info_t)(void);
```

---

## connector_ops_t (connector.h)

```c
typedef struct connector_ops_t {
    int  (*connect)(void* ctx, const char* uri);
    int  (*listen)(void* ctx, const char* host, uint16_t port);
    int  (*send_to)(void* ctx, conn_id_t id, const void* data, size_t size);
    void (*close)(void* ctx, conn_id_t id);
    void (*get_scheme)(void* ctx, char* buf, size_t buf_size);
    void (*get_name)(void* ctx, char* buf, size_t buf_size);
    void (*shutdown)(void* ctx);
    void* connector_ctx;
} connector_ops_t;

typedef int (*connector_init_t)(host_api_t* api, connector_ops_t** out);
```

---

## C++ SDK — IHandler

```cpp
#include <sdk/cpp/handler.hpp>

class ChatHandler : public gn::IHandler {
public:
    const char* get_plugin_name() const override { return "chat"; }

    const plugin_info_t* get_plugin_info() const override {
        static plugin_info_t info{"chat", 0x00010000, 200, {}, 0};
        return &info;
    }

    void on_init() override {
        set_supported_types({MSG_TYPE_CHAT});
    }

    void handle_message(const header_t* hdr, const endpoint_t* ep,
                         const void* payload, size_t size) override
    {
        // Прямой ответ:
        send_response(ep->peer_id, MSG_TYPE_CHAT, "pong", 4);

        // По URI:
        send("tcp://10.0.0.2:25565", MSG_TYPE_CHAT, payload, size);

        // По публичному ключу:
        conn_id_t peer = find_conn("a3f5c8d2...64hex...");
        if (peer != CONN_ID_INVALID)
            send_response(peer, MSG_TYPE_CHAT, payload, size);
    }

    propagation_t on_result(const header_t*, uint32_t) override {
        return PROPAGATION_CONSUMED;
    }

    void on_connection_state(const char* uri, conn_state_t state) override {
        if (state == STATE_ESTABLISHED)
            LOG_INFO("Connected: {}", uri);
    }

    void on_shutdown() override { }
};

HANDLER_PLUGIN(ChatHandler)
```

### Методы IHandler (protected)

```cpp
void send(const char* uri, uint32_t type, const void* data, size_t size);
void send_response(conn_id_t id, uint32_t type, const void* data, size_t size);
conn_id_t find_conn(const char* pubkey_hex_64) const;
int sign(const void* data, size_t size, uint8_t sig[64]);
int verify(const void* data, size_t size, const uint8_t* pubkey, const uint8_t* sig);
void log(int level, const char* file, int line, const char* msg);
void set_supported_types(std::initializer_list<uint32_t> types);
```

---

## C++ SDK — IConnector

```cpp
#include <sdk/cpp/connector.hpp>

class MyConnector : public gn::IConnector {
public:
    std::string get_scheme() const override { return "tcp"; }
    std::string get_name()   const override { return "My TCP"; }

    const plugin_info_t* get_plugin_info() const override {
        static plugin_info_t info{"my_tcp", 0x00010000, 128, {}, 0};
        return &info;
    }

    void on_init() override { /* io_context, acceptor */ }

    int do_connect(const char* uri) override {
        endpoint_t ep{};
        conn_id_t id = notify_connect(&ep);
        // async_connect, сохранить id
        return 0;
    }

    int do_listen(const char* host, uint16_t port) override {
        // acceptor loop → notify_connect() при каждом accept
        return 0;
    }

    int do_send_to(conn_id_t id, const void* data, size_t size) override {
        // async_write по сокету
        return 0;
    }

    void do_close(conn_id_t id) override {
        notify_disconnect(id, 0);
    }

    void on_shutdown() override { /* stop io_context */ }
};

CONNECTOR_PLUGIN(MyConnector)
```

### Методы IConnector (protected)

```cpp
conn_id_t notify_connect(const endpoint_t* ep);
void notify_data(conn_id_t id, const void* data, size_t size);
void notify_disconnect(conn_id_t id, int error = 0);
conn_id_t find_peer_conn(const char* pubkey_hex_64) const;
void register_extra_handler(handler_t* h);
void log(int level, const char* file, int line, const char* msg);
```

---

## PodData\<T\>

Zero-copy обёртка для POD-структур payload.

```cpp
#include <sdk/cpp/data.hpp>

#pragma pack(push, 1)
struct MyPayload {
    uint32_t id;
    uint8_t  flags;
};
#pragma pack(pop)

using MyMsg = gn::sdk::PodData<MyPayload>;

// Отправка:
MyMsg msg;
msg->id    = 42;
msg->flags = 0x01;
auto wire  = msg.serialize();  // RawBuffer = vector<uint8_t>
send_response(peer, MSG_TYPE_CHAT, wire.data(), wire.size());

// Приём:
auto parsed = gn::sdk::from_bytes<MyMsg>(payload, size);
LOG_DEBUG("id={} flags={}", parsed->id, parsed->flags);
```

---

## Цепочка ответственности

Несколько хендлеров на один тип — вызываются по убыванию `priority`:

```
security_handler (priority=255) → CONTINUE → передать дальше
chat_handler     (priority=200) → CONSUMED → стоп
logger_handler   (priority=  0) → не вызывается
```

---

## Макросы HANDLER_PLUGIN / CONNECTOR_PLUGIN

```cpp
#define HANDLER_PLUGIN(ClassName)
    static ClassName _gn_plugin_instance;
    extern "C" GN_EXPORT
    const plugin_info_t* plugin_get_info() {
        return _gn_plugin_instance.get_plugin_info();
    }
    extern "C" GN_EXPORT
    int handler_init(host_api_t* api, handler_t** out) {
        _gn_plugin_instance.init(api);
        *out = _gn_plugin_instance.to_c_handler();
        return 0;
    }
```

- `static` экземпляр — один на `.so`
- `plugin_get_info()` вызывается **до** `*_init()` для чтения метаданных
- `GN_EXPORT` — `__attribute__((visibility("default")))`

---

## SHA-256 манифест

Каждый плагин поставляется с `<plugin>.so.json`:

```json
{
  "meta": { "name": "logger", "version": "1.0.0" },
  "integrity": { "hash": "a3f5c8d2e1b04f..." }
}
```

`buildPlugin.nix` генерирует манифест автоматически. Потоковый хэш: 64KB чанки через `crypto_hash_sha256_*` (libsodium).

---

## Сборка плагина

### CMake

```cmake
add_library(my_handler SHARED my_handler.cpp)
target_link_libraries(my_handler PRIVATE goodnet_core)
```

### Nix

```nix
{ pkgs, mkCppPlugin, goodnetSdk }:
mkCppPlugin {
  name    = "my_handler";
  src     = ./.;
  sdk     = goodnetSdk;
  sources = [ "my_handler.cpp" ];
}
```

---

## Статическая линковка плагинов

По умолчанию GoodNet загружает плагины динамически через `dlopen()` / `LoadLibrary()`. Для портативных сборок, встраиваемых систем или Windows-развёртывания можно скомпилировать плагины прямо в бинарник.

### Как это работает

1. `sdk/static_registry.hpp` предоставляет глобальный реестр `gn::static_plugin_registry()`
2. При компиляции с `-DGOODNET_STATIC_PLUGINS` макросы `HANDLER_PLUGIN` / `CONNECTOR_PLUGIN` регистрируют плагин в этой таблице при статической инициализации (вместо экспорта `extern "C"` символов)
3. `PluginManager::load_static_plugins()` обходит реестр и инициализирует каждую запись точно так же, как динамически загруженный плагин

### Использование в CMake

```cmake
add_executable(myapp
    src/main.cpp
    plugins/connectors/tcp/tcp.cpp
    plugins/handlers/logger/logger.cpp
)
target_compile_definitions(myapp PRIVATE GOODNET_STATIC_PLUGINS)
target_link_libraries(myapp PRIVATE goodnet_core)
```

### Совместимость с динамической загрузкой

Оба режима совместимы — часть плагинов может быть статической, а остальные загружаются динамически во время выполнения. Статические плагины регистрируются при старте программы, а `load_all_plugins()` загружает дополнительные `.so` из директории плагинов.

---

*← [03 — Core API](03-core-api.md) · [05 — Системные сообщения →](05-system-messages.md)*
