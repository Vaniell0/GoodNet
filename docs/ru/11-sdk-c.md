# 11 — C SDK

`sdk/types.h` · `sdk/plugin.h` · `sdk/handler.h` · `sdk/connector.h`

C SDK — единственный интерфейс, который должен знать плагин на любом языке с C FFI.

---

## types.h

```c
// Версия протокола
#define GNET_MAGIC     0x474E4554U  // 'GNET'
#define GNET_PROTO_VER 1U

// Дескриптор соединения
typedef uint64_t conn_id_t;
#define CONN_ID_INVALID 0ULL

// Тип плагина (устанавливается PluginManager перед *_init)
typedef enum { PLUGIN_TYPE_UNKNOWN, PLUGIN_TYPE_HANDLER, PLUGIN_TYPE_CONNECTOR } plugin_type_t;

// Типы сообщений
#define MSG_TYPE_SYSTEM       0u
#define MSG_TYPE_AUTH         1u
#define MSG_TYPE_HEARTBEAT    3u
#define MSG_TYPE_CHAT       100u
#define MSG_TYPE_FILE       200u

// Состояния соединения
typedef enum {
    STATE_CONNECTING, STATE_AUTH_PENDING, STATE_KEY_EXCHANGE,
    STATE_ESTABLISHED, STATE_CLOSING, STATE_BLOCKED, STATE_CLOSED
} conn_state_t;

// Заголовок пакета (packed, 98 байт)
typedef struct {
    uint32_t magic;
    uint8_t  proto_ver;
    uint8_t  flags;
    uint16_t reserved;
    uint64_t packet_id;
    uint64_t timestamp;    // unix microseconds
    uint32_t payload_type;
    uint16_t status;
    uint32_t payload_len;
    uint8_t  signature[64]; // Ed25519; нули до ESTABLISHED
} header_t;

// Адрес пира
typedef struct {
    char     address[128]; // IP/hostname
    uint16_t port;
    uint8_t  pubkey[32];   // Ed25519 user pubkey (после AUTH)
    uint64_t peer_id;      // зарезервировано
} endpoint_t;

#define STATUS_OK    0
#define STATUS_ERROR 1
```

---

## host_api_t (plugin.h)

Единственный канал плагин ↔ ядро. Инъектируется через `*_init()`.

```c
typedef struct host_api_t {
    // Коннектор → ядро (обязательно вызывать при событиях):
    conn_id_t (*on_connect)   (void* ctx, const endpoint_t* ep);
    void      (*on_data)      (void* ctx, conn_id_t id, const void* raw, size_t size);
    void      (*on_disconnect)(void* ctx, conn_id_t id, int error_code);

    // Хендлер → ядро:
    void (*send)(void* ctx, const char* uri, uint32_t msg_type,
                 const void* payload, size_t size);

    // Крипто (приватный ключ не покидает ядро):
    int (*sign_with_device)(void* ctx, const void* data, size_t size, uint8_t sig[64]);
    int (*verify_signature)(void* ctx, const void* data, size_t size,
                            const uint8_t* pubkey, const uint8_t* signature);

    void*         internal_logger; // spdlog::logger* для sync_plugin_context()
    plugin_type_t plugin_type;     // установлен PluginManager перед *_init()
    void*         ctx;             // передавать первым аргументом во все коллбэки
} host_api_t;
```

---

## handler_t (handler.h)

```c
typedef struct {
    const char* name;  // уникальное имя, ключ для find_handler_by_name()

    void (*handle_message)(void* user_data, const header_t* hdr,
                           const endpoint_t* ep, const void* payload, size_t size);
    void (*handle_conn_state)(void* user_data, const char* uri, conn_state_t state);
    void (*shutdown)(void* user_data);

    const uint32_t* supported_types;     // NULL → wildcard
    size_t          num_supported_types;
    void*           user_data;
} handler_t;

typedef int (*handler_init_t)(host_api_t* api, handler_t** out_handler);
```

---

## connector_ops_t (connector.h)

```c
typedef struct connector_ops_t {
    int  (*connect) (void* ctx, const char* uri);
    int  (*listen)  (void* ctx, const char* host, uint16_t port);
    int  (*send_to) (void* ctx, conn_id_t id, const void* data, size_t size);
    void (*close)   (void* ctx, conn_id_t id);
    void (*get_scheme)(void* ctx, char* buf, size_t buf_size);
    void (*get_name)  (void* ctx, char* buf, size_t buf_size);
    void (*shutdown)  (void* ctx);
    void* connector_ctx;
} connector_ops_t;

typedef int (*connector_init_t)(host_api_t* api, connector_ops_t** out_ops);
```

---

*← [10 — Config](10-config.md) · [12 — C++ SDK →](12-sdk-cpp.md)*
