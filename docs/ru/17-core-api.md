# 17 — Core API

`include/core.hpp` · `include/core.h` · `src/core.cpp` · `src/capi.cpp`

`gn::Core` — главная точка входа в фреймворк для C++ кода. Инкапсулирует весь жизненный цикл: инициализацию, плагины, идентификацию, сетевой стек. Реализует паттерн **Pimpl**: заголовок не включает ни Boost, ни libsodium, ни spdlog — только `<memory>`, `<functional>`, `<string>`, `<filesystem>` и `sdk/types.h`. Downstream-код компилируется за миллисекунды.

---

## CoreConfig

Простая структура без зависимостей. Заполняется до создания `Core`.

```cpp
struct CoreConfig {
    struct {
        fs::path dir            = "~/.goodnet"; // директория ключей
        fs::path ssh_key_path;                  // путь к SSH Ed25519 ключу
        bool     use_machine_id = true;         // привязать device_key к железу
    } identity;

    struct {
        std::vector<fs::path> dirs;             // директории поиска плагинов
        bool auto_load = true;                  // загрузить всё при старте
    } plugins;

    struct {
        std::string listen_address = "0.0.0.0";
        uint16_t    listen_port    = 25565;
        int         io_threads     = 0;         // 0 = hardware_concurrency()
    } network;

    struct {
        std::string level     = "info";
        std::string file;                       // пусто = только stdout
        size_t      max_size  = 10 * 1024 * 1024;
        int         max_files = 5;
    } logging;

    fs::path config_file; // JSON-конфиг (опционально, загружается при старте)
};
```

---

## Последовательность инициализации (Core::Core)

```
Core::Core(CoreConfig cfg)
  │
  ├─ 1. Logger::log_level / log_file / max_size / max_files
  │       (до любого LOG_*)
  │
  ├─ 2. config.load_from_file(cfg.config_file)
  │       если файл задан и существует
  │
  ├─ 3. NodeIdentity::load_or_generate(IdentityConfig)
  │       expand_home("~/...") → абсолютный путь
  │       SSH ключ / генерация user_key
  │       MachineId → device_key
  │
  ├─ 4. ConnectionManager(bus, identity)
  │
  ├─ 5. cm.fill_host_api(&host_api)
  │       + host_api.internal_logger = Logger::get().get()
  │
  ├─ 6. PluginManager(&host_api, plugins.dirs[0])
  │
  └─ 7. auto_load:
          pm.load_all_plugins()           ← dirs[0]
          for dir in dirs[1..]:           ← дополнительные директории
              for *.so/*.dylib/*.dll:
                  pm.load_plugin(path)
          for connector in pm.get_active_connectors():
              cm.register_connector(scheme, connector)
          for handler in pm.get_active_handlers():
              cm.register_handler(handler)
```

Плагины из дополнительных директорий (`dirs[1..]`) загружаются вручную — `load_all_plugins()` сканирует только первую директорию.

---

## Lifecycle API

```cpp
gn::Core core(cfg);       // конструктор: полная инициализация

core.run();               // запустить IO потоки + заблокировать (до stop())
core.run_async(n);        // запустить n IO потоков в фоне (0 = auto)
core.stop();              // остановить всё, дождаться потоков
bool ok = core.is_running();
```

### stop() — последовательность

```
stop()
  ├─ pm.unload_all()           // shutdown() + dlclose() всех плагинов
  ├─ cm.shutdown()             // закрыть все соединения, дождаться drain
  ├─ work_guard.reset()        // разрешить io_context завершиться
  ├─ ioc.stop()
  └─ join(io_threads)
```

### run() vs run_async()

```cpp
// Синхронный режим — main thread блокируется:
core.run();

// Асинхронный режим — main thread свободен для CLI/UI:
core.run_async(4);  // 4 IO потока
while (core.is_running()) {
    handle_cli_input();
}
core.stop();
```

---

## Network API

```cpp
// Отправить данные по URI. Ядро разрешает маршрут, шифрует.
core.send("tcp://10.0.0.2:25565", MSG_TYPE_CHAT, data, size);
core.send("tcp://10.0.0.2:25565", MSG_TYPE_CHAT, "hello"sv);
```

---

## Subscription API

Позволяет подписаться на пакеты напрямую из кода, без плагина. Удобно для тестов и встраивания.

```cpp
using PacketHandler = std::function<propagation_t(
    std::string_view   handler_name,
    shared_ptr<header_t> hdr,
    const endpoint_t*    ep,
    PacketData           data        // shared_ptr<vector<uint8_t>>
)>;

// Конкретный тип сообщения:
core.subscribe(MSG_TYPE_CHAT, "my_listener",
    [](auto name, auto hdr, auto ep, auto data) {
        fmt::print("Got {} bytes from {}\n", data->size(), ep->address);
        return PROPAGATION_CONTINUE;
    }, 128 /*priority*/);

// Wildcard (все типы):
core.subscribe_wildcard("debug_logger",
    [](auto name, auto hdr, auto ep, auto data) {
        return PROPAGATION_CONTINUE;
    });
```

Callbacks выполняются в контексте IO-потоков через `PipelineSignal`. Не блокируйте в них надолго.

---

## Identity API

```cpp
std::string upk = core.user_pubkey_hex();   // 64 hex символа
std::string dpk = core.device_pubkey_hex(); // 64 hex символа
```

---

## Stats API

```cpp
size_t n = core.connection_count();
vector<string> uris = core.active_uris();  // список активных URI
```

---

## Internal Access (CLI / тесты)

```cpp
// Требуют включения соответствующих заголовков:
core.cm().rotate_identity_keys(new_cfg);   // ConnectionManager&
core.cm().stats().rx_bytes.load();         // StatsCollector
core.pm().list_plugins();                  // PluginManager&
core.bus().subscribe(...);                 // SignalBus&
```

---

## Pimpl гарантии

| Что скрыто за Pimpl | Почему |
|---|---|
| `boost::asio::io_context` + `work_guard` | Boost не нужен downstream |
| `SignalBus` | Тянет за собой `signals.hpp` |
| `ConnectionManager` | Тянет sodium, signals, data headers |
| `PluginManager` | Тянет dlopen, filesystem, nlohmann |
| `Logger` | Тянет spdlog |
| `NodeIdentity` | Тянет sodium |

---

## C API (core.h / capi.cpp)

Тонкая обёртка над `gn::Core` для использования из C, Python (ctypes), Rust (FFI), Go (cgo).

### Типы

```c
typedef struct gn_core_t gn_core_t; // непрозрачный дескриптор

typedef struct {
    const char* config_dir;  // путь к директории ключей (NULL = "~/.goodnet")
    const char* log_level;   // "trace"|"debug"|"info"|"warn"|"error" (NULL = "info")
    uint16_t    listen_port; // 0 = дефолт (25565)
} gn_config_t;
```

### Функции

```c
// ── Lifecycle ─────────────────────────────────────────────────────────────────
gn_core_t* gn_core_create(gn_config_t* cfg);   // cfg == NULL → defaults
void       gn_core_destroy(gn_core_t* core);
void       gn_core_run(gn_core_t* core);        // блокирует до stop()
void       gn_core_run_async(gn_core_t* core, int threads); // 0 = auto
void       gn_core_stop(gn_core_t* core);

// ── Network ───────────────────────────────────────────────────────────────────
void gn_core_send(gn_core_t* core, const char* uri,
                  uint32_t type, const void* data, size_t len);

// ── Identity ──────────────────────────────────────────────────────────────────
// Копирует hex-строку в buffer. Возвращает длину (без NUL).
size_t gn_core_get_user_pubkey(gn_core_t* core,
                                char* buffer, size_t max_len);

// ── Subscriptions ─────────────────────────────────────────────────────────────
// Callback-тип: возвращает 0=CONTINUE, 1=CONSUMED, 2=REJECT
typedef int (*gn_handler_t)(uint32_t type, const void* data,
                             size_t len, void* user_data);

void gn_core_subscribe(gn_core_t* core, uint32_t type,
                        gn_handler_t callback, void* user_data);
```

### Пример (C)

```c
gn_config_t cfg = {
    .config_dir  = "/etc/myapp/keys",
    .log_level   = "info",
    .listen_port = 8080,
};

gn_core_t* core = gn_core_create(&cfg);
gn_core_run_async(core, 0);

char pubkey[65];
gn_core_get_user_pubkey(core, pubkey, sizeof(pubkey));
printf("User pubkey: %s\n", pubkey);

gn_core_send(core, "tcp://10.0.0.1:8080", 100, "hello", 5);

gn_core_stop(core);
gn_core_destroy(core);
```

### Пример (Python ctypes)

```python
import ctypes, os
lib = ctypes.CDLL("libgoodnet_core.so")

lib.gn_core_create.restype  = ctypes.c_void_p
lib.gn_core_create.argtypes = [ctypes.c_void_p]
lib.gn_core_run_async.argtypes = [ctypes.c_void_p, ctypes.c_int]
lib.gn_core_stop.argtypes      = [ctypes.c_void_p]
lib.gn_core_destroy.argtypes   = [ctypes.c_void_p]

core = lib.gn_core_create(None)
lib.gn_core_run_async(core, 0)
# ... работаем ...
lib.gn_core_stop(core)
lib.gn_core_destroy(core)
```

---

## Известные ограничения (alpha)

`gn_core_subscribe()` объявлен в `core.h`, но реализация `capi.cpp` на момент alpha не содержит полного моста `gn_handler_t → PipelineSignal`. Для подписок из C++ используйте `Core::subscribe()` напрямую. Для внешних языков — реализуйте обёртку через Handler-плагин.

---

*← [16 — Roadmap](16-roadmap.md) · [README →](README.md)*
