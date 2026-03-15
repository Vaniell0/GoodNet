# 03 — Core API

`include/core.hpp` · `include/core.h` · `src/core.cpp` · `src/capi.cpp`

`gn::Core` — главная точка входа в фреймворк. Инкапсулирует жизненный цикл: инициализацию, плагины, идентификацию, сетевой стек. Реализует **Pimpl**: заголовок не включает Boost, libsodium, spdlog — только stdlib и `sdk/types.h`. Downstream-код компилируется за миллисекунды.

---

## CoreConfig

Структура без зависимостей. Заполняется до создания `Core`.

```cpp
struct CoreConfig {
    struct {
        fs::path dir            = "~/.goodnet";  // директория ключей
        fs::path ssh_key_path;                    // путь к SSH Ed25519 ключу
        bool     use_machine_id = true;           // привязать device_key к железу
    } identity;

    struct {
        std::vector<fs::path> dirs;               // директории поиска плагинов
        bool auto_load = true;                    // загрузить всё при старте
    } plugins;

    struct {
        std::string listen_address = "0.0.0.0";
        uint16_t    listen_port    = 25565;
        int         io_threads     = 0;           // 0 = hardware_concurrency()
    } network;

    struct {
        std::string level     = "info";           // trace|debug|info|warn|error
        std::string file;                         // пусто = только logs/goodnet.log
        size_t      max_size  = 10 * 1024 * 1024; // ротация: 10 MB
        int         max_files = 5;
    } logging;

    fs::path config_file;                         // JSON-конфиг (опционально)
};
```

---

## Последовательность инициализации

```
Core::Core(CoreConfig cfg)
  │
  ├─ 1. Logger — уровень, файл, ротация (до любого LOG_*)
  ├─ 2. Config::load_from_file(cfg.config_file)
  ├─ 3. NodeIdentity::load_or_generate()
  │       expand_home("~/...") → SSH ключ / генерация user_key → MachineId → device_key
  ├─ 4. ConnectionManager(bus, identity)
  ├─ 5. fill_host_api(&host_api) + internal_logger
  ├─ 6. PluginManager(&host_api, plugins.dirs[0])
  └─ 7. auto_load:
          pm.load_all_plugins()        ← dirs[0]
          for dir in dirs[1..]:        ← дополнительные
              pm.load_plugin(*.so)
          register_connector(scheme, ops) для каждого коннектора
```

---

## Lifecycle API

```cpp
gn::Core core(cfg);          // полная инициализация

core.run();                   // IO потоки + блокирует до stop()
core.run_async(4);            // 4 IO потока в фоне (0 = auto)
core.stop();                  // shutdown плагинов → закрыть соединения → join
bool ok = core.is_running();
```

### stop() — последовательность

```
stop()
  ├─ pm.unload_all()       // shutdown() + dlclose() всех плагинов
  ├─ cm.shutdown()          // закрыть соединения, drain
  ├─ work_guard.reset()     // разрешить io_context завершиться
  ├─ ioc.stop()
  └─ join(io_threads)
```

### run() vs run_async()

```cpp
// Синхронный — main thread блокируется:
core.run();

// Асинхронный — main thread свободен:
core.run_async(4);
while (core.is_running()) {
    handle_cli_input();
}
core.stop();
```

---

## Network API

```cpp
// По URI (ядро разрешает маршрут, шифрует):
core.send("tcp://10.0.0.2:25565", MSG_TYPE_CHAT, data, size);
core.send("tcp://10.0.0.2:25565", MSG_TYPE_CHAT, "hello"sv);
core.send("tcp://10.0.0.2:25565", MSG_TYPE_CHAT, std::span{bytes});

// По conn_id (без поиска по URI):
core.send_to(conn_id, MSG_TYPE_CHAT, data, size);
core.send_to(conn_id, MSG_TYPE_CHAT, "hello"sv);
core.send_to(conn_id, MSG_TYPE_CHAT, std::span{bytes});

// Broadcast — всем ESTABLISHED соединениям:
core.broadcast(MSG_TYPE_CHAT, data, size);
core.broadcast(MSG_TYPE_CHAT, "hello"sv);

// Управление соединениями:
core.connect("tcp://10.0.0.3:25565");   // инициировать подключение
core.disconnect(conn_id);                // graceful close
core.close_now(conn_id);                 // immediate close
```

---

## Subscription API

Подписка на пакеты напрямую из кода, без плагина. Удобно для тестов и встраивания.

```cpp
using PacketHandler = std::function<propagation_t(
    std::string_view          name,
    std::shared_ptr<header_t> hdr,
    const endpoint_t*         ep,
    PacketData                data    // shared_ptr<vector<uint8_t>>
)>;

// Конкретный тип:
uint64_t sub_id = core.subscribe(1000, "my_listener",
    [](auto name, auto hdr, auto ep, auto data) {
        fmt::print("Got {} bytes from {}\n", data->size(), ep->address);
        return PROPAGATION_CONSUMED;
    }, 128 /*priority*/);

// Wildcard (все типы):
core.subscribe_wildcard("debug_logger",
    [](auto name, auto hdr, auto ep, auto data) {
        return PROPAGATION_CONTINUE;
    });

// Отписка:
core.unsubscribe(sub_id);
```

Callbacks выполняются в контексте IO-потоков через `PipelineSignal`. Не блокируйте в них надолго.

---

## Key Management

```cpp
// Ротация session_key без разрыва соединения (PFS):
bool ok = core.rekey_session(conn_id);

// Ротация identity keys (существующие сессии не затрагиваются):
core.rotate_identity_keys();
```

---

## Peer Info

```cpp
// Публичный ключ пира (32 байта, Ed25519 user_key):
std::vector<uint8_t> pk = core.peer_pubkey(conn_id);

// Endpoint пира:
endpoint_t ep;
if (core.peer_endpoint(conn_id, ep))
    fmt::print("Peer: {}:{}\n", ep.address, ep.port);
```

---

## Identity API

```cpp
std::string upk = core.user_pubkey_hex();    // 64 hex символа
std::string dpk = core.device_pubkey_hex();  // 64 hex символа
```

---

## Stats API

```cpp
size_t n = core.connection_count();
std::vector<std::string> uris = core.active_uris();
std::vector<conn_id_t> ids = core.active_conn_ids();

StatsSnapshot snap = core.stats_snapshot();
```

### StatsSnapshot

```cpp
struct StatsSnapshot {
    uint64_t rx_bytes, tx_bytes;
    uint64_t rx_packets, tx_packets;
    uint64_t auth_ok, auth_fail;
    uint64_t decrypt_fail, backpressure;
    uint64_t consumed, rejected;
    uint32_t connections;              // текущие ESTABLISHED
    uint32_t total_conn, total_disc;   // кумулятивные
    uint64_t drops[15];                // по DropReason
    LatencyHistogram dispatch_latency; // гистограмма задержек
};
```

### DropReason

```cpp
enum class DropReason : uint8_t {
    BadMagic, BadProtoVer, ConnNotFound,
    StateNotEstablished, AuthFail, DecryptFail,
    ReplayDetected, Backpressure, PerConnLimitExceeded,
    SessionNotReady, RejectedByHandler, ShuttingDown,
    RelayDropped, SenderIdMismatch, TrustedFromRemote
};
```

### LatencyHistogram

```cpp
struct LatencyHistogram {
    // Бакеты: <1μs, <10μs, <100μs, <1ms, <10ms, <100ms, >100ms
    std::atomic<uint64_t> buckets[7];
    std::atomic<uint64_t> total_ns, count;
    uint64_t avg_ns() const noexcept;
};
```

---

## Config API

```cpp
bool ok = core.reload_config();  // перечитать JSON-файл
```

---

## Internal Access (CLI / тесты)

```cpp
core.cm();    // ConnectionManager&
core.pm();    // PluginManager&
core.bus();   // SignalBus&
```

---

## C API (`core.h`)

Тонкая обёртка для C, Python (ctypes), Rust (FFI), Go (cgo).

### Типы

```c
typedef struct gn_core_t gn_core_t;  // непрозрачный дескриптор

typedef struct {
    const char* config_dir;   // NULL = "~/.goodnet"
    const char* log_level;    // NULL = "info"
    uint16_t    listen_port;  // 0 = 25565
} gn_config_t;

typedef struct {
    uint64_t rx_bytes, tx_bytes;
    uint64_t rx_packets, tx_packets;
    uint64_t auth_ok, auth_fail;
    uint64_t decrypt_fail, backpressure;
    uint64_t consumed, rejected;
    uint32_t connections, total_conn, total_disc;
    uint64_t drops[15];
    uint64_t dispatch_lat_avg;
} gn_stats_t;
```

### Функции

```c
// Lifecycle
gn_core_t* gn_core_create(gn_config_t* cfg);  // NULL → defaults
void       gn_core_destroy(gn_core_t* core);
void       gn_core_run(gn_core_t* core);
void       gn_core_run_async(gn_core_t* core, int threads);
void       gn_core_stop(gn_core_t* core);
int        gn_core_reload_config(gn_core_t* core);

// Network
void gn_core_send(gn_core_t* core, const char* uri,
                   uint32_t type, const void* data, size_t len);
void gn_core_broadcast(gn_core_t* core, uint32_t type,
                        const void* data, size_t len);
void gn_core_disconnect(gn_core_t* core, uint64_t conn_id);
int  gn_core_rekey(gn_core_t* core, uint64_t conn_id);

// Identity
size_t gn_core_get_user_pubkey(gn_core_t* core, char* buf, size_t max);

// Stats
void gn_core_get_stats(gn_core_t* core, gn_stats_t* out);

// Subscriptions
typedef propagation_t (*gn_handler_fn)(uint32_t type, const void* data,
                                        size_t len, void* user_data);
uint64_t gn_core_subscribe(gn_core_t* core, uint32_t type,
                             gn_handler_fn cb, void* user_data);
void     gn_core_unsubscribe(gn_core_t* core, uint64_t sub_id);
```

### Пример (C)

```c
gn_config_t cfg = { .config_dir = "/etc/myapp/keys", .listen_port = 8080 };
gn_core_t* core = gn_core_create(&cfg);
gn_core_run_async(core, 0);

char pubkey[65];
gn_core_get_user_pubkey(core, pubkey, sizeof(pubkey));
printf("User pubkey: %s\n", pubkey);

gn_core_send(core, "tcp://10.0.0.1:8080", 100, "hello", 5);

gn_stats_t stats;
gn_core_get_stats(core, &stats);
printf("RX: %lu bytes, TX: %lu bytes\n", stats.rx_bytes, stats.tx_bytes);

gn_core_stop(core);
gn_core_destroy(core);
```

---

*← [02 — Сборка](02-build.md) · [04 — Плагины →](04-plugins.md)*
