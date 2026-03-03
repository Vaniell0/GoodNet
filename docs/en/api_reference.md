# GoodNet — API Reference

## C Types (`sdk/types.h`, `sdk/handler.h`, `sdk/connector.h`, `sdk/plugin.h`)

These headers define the ABI boundary between the core and plugins. They are pure C and can be included from C or C++ code.

---

### `header_t`

Wire format packet header. `#pragma pack(push, 1)` — no padding, safe for network transmission.

```c
typedef struct {
    uint32_t magic;        // Must be GNET_MAGIC (0x474E4554)
    uint64_t packet_id;    // Monotonically increasing ID
    uint64_t timestamp;    // Unix timestamp (microseconds)
    uint32_t payload_type; // Application-level message type (see MSG_TYPE_* constants)
    uint16_t status;       // STATUS_OK (0) or STATUS_ERROR (1)
    uint16_t reserved;     // Must be zero
    uint32_t payload_len;  // Byte length of the payload following this header
} header_t;
```

Constants for `payload_type`:

| Constant | Value | Meaning |
|---|---|---|
| `MSG_TYPE_SYSTEM` | 0 | System / wildcard (received by all handlers) |
| `MSG_TYPE_AUTH` | 1 | Authentication |
| `MSG_TYPE_KEY_EXCHANGE` | 2 | Cryptographic key exchange |
| `MSG_TYPE_HEARTBEAT` | 3 | Keep-alive |
| `MSG_TYPE_CHAT` | 100 | Chat message |
| `MSG_TYPE_FILE` | 200 | File transfer |

---

### `endpoint_t`

Describes the remote peer of a connection.

```c
typedef struct {
    char     address[128]; // Null-terminated IP address string
    uint16_t port;         // Remote port
    uint64_t peer_id;      // Application-level peer identifier
} endpoint_t;
```

---

### `conn_state_t`

```c
typedef enum {
    STATE_CONNECTING,
    STATE_AUTH_PENDING,
    STATE_KEY_EXCHANGE,
    STATE_ESTABLISHED,
    STATE_CLOSING,
    STATE_BLOCKED,
    STATE_CLOSED
} conn_state_t;
```

---

### `host_api_t`

The struct passed to every plugin on initialization. Gives plugins access to core services.

```c
typedef struct {
    void*  internal_logger;   // Raw spdlog::logger* — used by sync_plugin_context()
    void (*send)(const char* uri, uint32_t type,
                 const void* data, size_t size);
    handle_t (*create_connection)(const char* uri);
    void     (*close_connection)(handle_t handle);
    void     (*update_connection_state)(const char* uri, conn_state_t state);
    plugin_type_t plugin_type; // Set by PluginManager before calling init
} host_api_t;
```

`plugin_type` is set to `PLUGIN_TYPE_HANDLER` or `PLUGIN_TYPE_CONNECTOR` before the entry point is called, allowing a plugin to verify it was loaded as the correct type.

---

### `handler_t`

The C-level handle returned by `handler_init`. `PluginManager` stores a pointer to this and calls its function pointers during packet dispatch.

```c
typedef struct {
    void (*handle_message)(void* user_data, const header_t* header,
                           const endpoint_t* endpoint,
                           const void* payload, size_t payload_size);
    void (*handle_conn_state)(void* user_data, const char* uri, conn_state_t state);
    void (*shutdown)(void* user_data);
    uint32_t* supported_types;     // Array of accepted payload_type values
    size_t    num_supported_types; // Length of supported_types (0 = accept all)
    void*     user_data;           // Passed back as first arg to every callback
} handler_t;
```

---

### `connector_ops_t`

```c
typedef struct {
    connection_ops_t* (*connect)(void* ctx, const char* uri);
    int  (*listen)(void* ctx, const char* host, uint16_t port);
    void (*get_scheme)(void* ctx, char* buf, size_t size); // e.g. "tcp"
    void (*get_name)(void* ctx, char* buf, size_t size);   // e.g. "TCP Connector"
    void (*shutdown)(void* ctx);
    void* connector_ctx;
} connector_ops_t;
```

---

## C++ Classes (`include/`, `core/`, `sdk/cpp/`)

---

### `Logger`

**Header:** `include/logger.hpp`  
**Implementation:** `src/logger.cpp`

Thread-safe Meyers Singleton logger. Configure static members before the first `LOG_*` call; the instance is created lazily.

```cpp
class Logger {
public:
    // Configuration (set before first LOG_* call)
    static std::string log_level;        // default: "info"
    static std::string log_file;         // default: "logs/goodnet.log"
    static size_t      max_size;         // default: 10 MB
    static int         max_files;        // default: 5
    static std::string project_root;     // set from GOODNET_PROJECT_ROOT cmake define
    static bool        strip_extension;  // default: false
    static int         source_detail_mode; // default: 0 (auto)

    // Lifecycle
    static std::shared_ptr<spdlog::logger> get();
    static void shutdown();
    static void set_external_logger(std::shared_ptr<spdlog::logger> ext);

    // Logging (used via macros, not directly)
    template<typename... Args>
    static void info(std::string_view file, int line,
                     fmt::format_string<Args...>, Args&&...);
    // ... trace, debug, warn, error, critical (same signature)
};
```

`spdlog/spdlog.h` is **not** exposed through `logger.hpp`. Only `spdlog/common.h` is included. This keeps compilation fast for all TUs that include the logger header.

---

### `PluginManager`

**Header:** `core/pluginManager.hpp`  
**Implementation:** `core/pluginManager_core.cpp`, `_state.cpp`, `_lookup.cpp`

Owns the lifecycle of all loaded plugins. Thread-safe: uses a `std::shared_mutex` (read for lookups, write for load/unload).

```cpp
class PluginManager {
public:
    explicit PluginManager(host_api_t* api, fs::path plugins_base_dir = "");
    ~PluginManager();  // calls unload_all()

    // Loading
    std::expected<void, std::string> load_plugin(const fs::path& path);
    void load_all_plugins();  // scans handlers/ and connectors/ subdirectories

    // Lookup (shared lock — zero-copy, safe to call from multiple threads)
    std::optional<handler_t*>       find_handler_by_name(std::string_view name) const;
    std::optional<connector_ops_t*> find_connector_by_scheme(std::string_view scheme) const;
    std::vector<handler_t*>         get_active_handlers() const;

    // State management (unique lock)
    bool unload_handler (std::string_view name);
    bool enable_handler (std::string_view name);
    bool disable_handler(std::string_view name);
    void unload_all();

    // Stats
    size_t get_enabled_handler_count() const;
    size_t get_enabled_connector_count() const;
    void   list_plugins() const;
};
```

Plugin discovery checks for a `.so.json` integrity manifest before calling `dlopen`. Plugins without a valid manifest are rejected.

---

### `gn::Signal<Args...>` / `PacketSignal`

**Header:** `include/signals.hpp`

A type-safe, Boost.Asio-backed signal that dispatches to multiple handlers asynchronously via a strand.

```cpp
template<typename... Args>
class Signal {
public:
    explicit Signal(boost::asio::io_context& ioc);

    template<typename Func>
    requires SignalHandler<Func, Args...>
    void connect(Func&& handler);

    void emit(Args... args);     // posts all handlers to the strand
    void disconnect_all();
    [[nodiscard]] size_t size() const;
};

// Concrete type used for packet routing
using PacketSignal = Signal<
    const header_t*,
    const endpoint_t*,
    std::span<const char>
>;
```

`emit()` posts work to a `boost::asio::strand`. Handlers execute serially on the thread pool, preventing data races without explicit locking in the hot path.

---

### `gn::IHandler`

**Header:** `sdk/cpp/handler.hpp`

C++ base class for handler plugins.

```cpp
class IHandler {
public:
    virtual void on_init()    {}
    virtual void on_shutdown() {}

    virtual void handle_message(
        const header_t*  header,
        const endpoint_t* endpoint,
        const void*      payload,
        size_t           payload_size) = 0;

    virtual void handle_connection_state(const char* uri, conn_state_t state) {}

    void set_supported_types(const std::vector<uint32_t>& types);
    handler_t* to_c_handler();  // called by HANDLER_PLUGIN macro

protected:
    host_api_t* api_ = nullptr;
    void send(const char* uri, uint32_t type, const void* data, size_t size);
};
```

---

### `gn::IConnector` / `gn::IConnection`

**Header:** `sdk/cpp/connector.hpp`

```cpp
class IConnector {
public:
    virtual void on_init()     {}
    virtual void on_shutdown() {}

    virtual std::unique_ptr<IConnection> create_connection(const std::string& uri) = 0;
    virtual bool        start_listening(const std::string& host, uint16_t port) = 0;
    virtual std::string get_scheme() const = 0;
    virtual std::string get_name()   const = 0;

    connector_ops_t* to_c_ops();  // called by CONNECTOR_PLUGIN macro
protected:
    host_api_t* api_ = nullptr;
};

class IConnection {
public:
    virtual bool       do_send(const void* data, size_t size) = 0;
    virtual void       do_close() = 0;
    virtual bool       is_connected() const = 0;
    virtual endpoint_t get_remote_endpoint() const = 0;
    virtual std::string get_uri_string() const = 0;

    void notify_data(const void* data, size_t size);
    void notify_close();
    void notify_error(int error_code);

    connection_ops_t* to_c_ops();
};
```

---

### `gn::Stats`

**Header:** `include/stats.hpp`

Flat aggregate of atomic runtime counters. All members are `static inline` — no instance needed.

```cpp
struct Stats {
    static inline std::atomic<size_t> packets_sent     = 0;
    static inline std::atomic<size_t> packets_received = 0;
    static inline std::atomic<size_t> connection_count = 0;
    static inline std::atomic<bool>   is_running       = false;
    static inline std::atomic<bool>   is_initialized   = false;
    static inline std::chrono::system_clock::time_point start_time;
    // ... see header for full list
};
```

---

## Message Type Constants

```c
#define MSG_TYPE_SYSTEM       0
#define MSG_TYPE_AUTH         1
#define MSG_TYPE_KEY_EXCHANGE 2
#define MSG_TYPE_HEARTBEAT    3
#define MSG_TYPE_CHAT       100
#define MSG_TYPE_FILE       200

#define GNET_MAGIC  0x474E4554U  // "GNET" in ASCII
#define STATUS_OK   0
#define STATUS_ERROR 1
```
