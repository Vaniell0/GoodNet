# 12 — C++ SDK

`sdk/cpp/handler.hpp` · `sdk/cpp/connector.hpp` · `sdk/cpp/data.hpp` · `sdk/cpp/plugin.hpp`

---

## IHandler

```cpp
class MyHandler : public gn::IHandler {
public:
    const char* get_plugin_name() const override { return "my_handler"; }

    void on_init() override {
        set_supported_types({MSG_TYPE_CHAT, MSG_TYPE_FILE});
        // empty list = wildcard (all types)
    }

    void handle_message(const header_t*   hdr,
                        const endpoint_t* ep,
                        const void*       payload,
                        size_t            size) override
    {
        LOG_INFO("msg type={} from {}:{}", hdr->payload_type, ep->address, ep->port);

        std::string reply = "pong";
        send(ep->address, MSG_TYPE_CHAT, reply.data(), reply.size());

        uint8_t sig[64];
        sign(payload, size, sig);   // Ed25519 with core's device_seckey
    }

    void handle_connection_state(const char* uri, conn_state_t st) override {
        if (st == STATE_ESTABLISHED) LOG_INFO("Connected: {}", uri);
    }

    void on_shutdown() override { /* cleanup */ }
};

HANDLER_PLUGIN(MyHandler)  // ← end of .cpp
```

### Helper methods

```cpp
// protected in IHandler:
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

    int do_connect(const char* uri) override {
        // resolve + async_connect
        // on success:
        endpoint_t ep{};
        conn_id_t id = notify_connect(&ep);  // register with core
        // store id next to socket
        return 0;
    }

    int do_listen(const char* host, uint16_t port) override {
        // setup acceptor; for each accept: notify_connect(&ep)
        return 0;
    }

    int do_send_to(conn_id_t id, const void* data, size_t size) override {
        // find socket by id, async_write
        return 0;
    }

    void do_close(conn_id_t id) override {
        // close socket
        notify_disconnect(id, 0);
    }
};

CONNECTOR_PLUGIN(MyConnector)
```

---

## PodData\<T\>

Zero-copy wrapper for POD payload structs.

```cpp
#pragma pack(push, 1)
struct HeartbeatPayload { uint32_t seq; uint8_t flags; };
#pragma pack(pop)
using HeartbeatMsg = gn::sdk::PodData<HeartbeatPayload>;

// Sending:
HeartbeatMsg ping;
ping->seq = ++counter; ping->flags = 0x00;
auto wire = ping.serialize();
send(uri, MSG_TYPE_HEARTBEAT, wire.data(), wire.size());

// Receiving:
auto pong = gn::sdk::from_bytes<HeartbeatMsg>(payload, size);
LOG_DEBUG("seq={}", pong->seq);
```

---

## Plugin Macros

```cpp
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

- `static instance` — one object per `.so`, Meyers singleton guarantee
- `GN_EXPORT` — `__attribute__((visibility("default")))` or `__declspec(dllexport)`
- `plugin_type` guard — prevents loading a Handler as a Connector

---

*← [11 — C SDK](11-sdk-c.md) · [13 — Build →](13-build.md)*
