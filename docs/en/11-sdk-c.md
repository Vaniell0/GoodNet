# 11 — C SDK

`sdk/types.h` · `sdk/plugin.h` · `sdk/handler.h` · `sdk/connector.h`

The C SDK is the only interface a plugin needs to know — usable from any language with C FFI.

---

## types.h — Core Types

```c
#define GNET_MAGIC     0x474E4554U  // 'GNET'
#define GNET_PROTO_VER 2U

typedef uint64_t conn_id_t;
#define CONN_ID_INVALID 0ULL

typedef enum { PLUGIN_TYPE_UNKNOWN, PLUGIN_TYPE_HANDLER, PLUGIN_TYPE_CONNECTOR } plugin_type_t;

#define MSG_TYPE_SYSTEM    0u
#define MSG_TYPE_AUTH      1u
#define MSG_TYPE_HEARTBEAT 3u
#define MSG_TYPE_CHAT    100u
#define MSG_TYPE_FILE    200u

typedef enum {
    STATE_CONNECTING, STATE_AUTH_PENDING, STATE_KEY_EXCHANGE,
    STATE_ESTABLISHED, STATE_CLOSING, STATE_BLOCKED, STATE_CLOSED
} conn_state_t;

// Packed header v2: 44 bytes, no padding
typedef struct {
    uint32_t magic; uint8_t proto_ver; uint8_t flags;
    uint16_t payload_type; uint32_t payload_len;
    uint64_t packet_id; uint64_t timestamp;
    uint8_t  sender_id[16];
} header_t;

typedef struct {
    char     address[128]; uint16_t port;
    uint8_t  pubkey[32];   uint64_t peer_id;
} endpoint_t;
```

---

## host_api_t — Plugin ↔ Core Channel

```c
typedef struct host_api_t {
    // Connector → Core (must call on transport events):
    conn_id_t (*on_connect)   (void* ctx, const endpoint_t* ep);
    void      (*on_data)      (void* ctx, conn_id_t id, const void* raw, size_t sz);
    void      (*on_disconnect)(void* ctx, conn_id_t id, int error_code);

    // Handler → Core:
    void (*send)(void* ctx, const char* uri, uint32_t type,
                 const void* payload, size_t size);

    // Crypto (private key never leaves core):
    int (*sign_with_device)(void* ctx, const void* data, size_t sz, uint8_t sig[64]);
    int (*verify_signature)(void* ctx, const void* data, size_t sz,
                            const uint8_t* pubkey, const uint8_t* sig);

    void*         internal_logger;
    plugin_type_t plugin_type;   // set by PluginManager before *_init()
    void*         ctx;           // pass as first arg to all callbacks
} host_api_t;
```

---

## handler_t

```c
typedef struct {
    const char* name;  // unique key for find_handler_by_name()

    void (*handle_message)(void* user_data, const header_t* hdr,
                           const endpoint_t* ep, const void* payload, size_t sz);
    void (*handle_conn_state)(void* user_data, const char* uri, conn_state_t st);
    void (*shutdown)(void* user_data);

    const uint32_t* supported_types;  // NULL → wildcard
    size_t          num_supported_types;
    void*           user_data;
} handler_t;

typedef int (*handler_init_t)(host_api_t* api, handler_t** out);
```

---

## connector_ops_t

```c
typedef struct connector_ops_t {
    int  (*connect) (void* ctx, const char* uri);
    int  (*listen)  (void* ctx, const char* host, uint16_t port);
    int  (*send_to) (void* ctx, conn_id_t id, const void* data, size_t sz);
    void (*close)   (void* ctx, conn_id_t id);
    void (*get_scheme)(void* ctx, char* buf, size_t buf_size);
    void (*get_name)  (void* ctx, char* buf, size_t buf_size);
    void (*shutdown)  (void* ctx);
    void* connector_ctx;
} connector_ops_t;

typedef int (*connector_init_t)(host_api_t* api, connector_ops_t** out);
```

---

*← [10 — Config](10-config.md) · [12 — C++ SDK →](12-sdk-cpp.md)*
