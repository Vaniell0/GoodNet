# 09 — Logger

`include/logger.hpp` · `src/logger.cpp`

---

## Архитектура

Logger реализован как **Meyers Singleton** поверх `spdlog`. Тяжёлые заголовки (`spdlog/spdlog.h`) включены только в `logger.cpp` — все остальные TU включают только `logger.hpp` и получают лёгкие forward declarations.

```
Logger (static class)
  ├── get_instance() → static shared_ptr<spdlog::logger>  (Meyers Singleton)
  ├── ensure_initialized() → std::call_once → init_internal()
  │
  ├── Sinks:
  │   ├── rotating_file_sink_mt  (всегда: Release + Debug)
  │   └── stdout_color_sink_mt   (только Debug, #ifndef NDEBUG)
  │
  ├── Custom flag %Q → custom_source_flag
  │   формат: [путь/к/файлу.cpp:строка]
  │
  └── shutdown() → flush → spdlog::drop_all → shared_ptr.reset()
```

### Почему Meyers Singleton, а не static member?

Static member инициализируется вместе с другими статиками своего TU, и порядок между TU не определён (Static Initialization Order Fiasco). Meyers static local инициализируется при **первом вызове** — гарантированно после `main()`. При `Logger::shutdown()`: `instance.reset()` → при `__do_global_dtors_aux` local видит nullptr → никакого повторного вызова деструктора.

---

## Конфигурация

Выставляется **до первого** `LOG_*`:

```cpp
Logger::log_level          = "debug";           // trace|debug|info|warn|error|critical|off
Logger::log_file           = "logs/goodnet.log";
Logger::max_size           = 10 * 1024 * 1024;  // 10 MB
Logger::max_files          = 5;                  // количество ротаций
Logger::project_root       = "/home/user/proj/"; // обрезать из путей в логе
Logger::strip_extension    = false;              // убрать .cpp из имени файла
Logger::source_detail_mode = 0;                  // 0=авто 1=путь+строка 2=файл+строка 3=файл
Logger::file_pattern       = "[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %Q%v";
Logger::console_pattern    = "%^[%H:%M:%S.%e] [%l] %Q%$%v";
```

`project_root` автоматически подставляется из CMake через `-DGOODNET_PROJECT_ROOT="${CMAKE_SOURCE_DIR}/"`.

---

## Флаг %Q — форматирование источника

| mode | trace / debug | info / warn / error |
|---|---|---|
| 0 (авто) | `[src/core/cm_session.cpp:42] ` | `[cm_session.cpp] ` |
| 1 (полный) | `[src/core/cm_session.cpp:42] ` | `[src/core/cm_session.cpp:42] ` |
| 2 (файл+строка) | `[cm_session.cpp:42] ` | `[cm_session.cpp:42] ` |
| 3 (только файл) | `[cm_session.cpp] ` | `[cm_session.cpp] ` |

---

## Макросы

### Всегда активны (Release + Debug)

```cpp
LOG_INFO("Loaded plugin: {} v{}", name, version);
LOG_WARN("Config key '{}' missing, using default: {}", key, def);
LOG_ERROR("Decrypt MAC failed: nonce={}", nonce);
LOG_CRITICAL("libsodium init failed — cannot continue");

INFO_VALUE(connection_count);   // → "connection_count = 5"
WARN_VALUE(timeout_ms);
ERROR_VALUE(conn_id);
INFO_POINTER(session_ptr);      // → "session_ptr [0x7f... valid:true]"
```

### Только в Debug (#ifndef NDEBUG)

```cpp
LOG_TRACE("Enter: {}", __FUNCTION__);
LOG_DEBUG("Recv buf size={} conn_id={}", buf.size(), id);

TRACE_VALUE(session_key_hex);
DEBUG_VALUE(recv_buf_size);
TRACE_POINTER(handler_ptr);

LOG_SCOPED_TRACE();   // ">>> my_func" при входе, "<<< my_func" при выходе
LOG_SCOPED_DEBUG();
```

В Release: `LOG_TRACE` / `LOG_DEBUG` / `TRACE_*` / `DEBUG_*` → `((void)0)`.

---

## Плагины и Logger

Плагины загружаются через `RTLD_LOCAL` — Logger не просачивается автоматически.

```cpp
// PluginManager перед вызовом *_init():
info->api_c.internal_logger = Logger::get_raw_ptr();

// В плагине (через sdk/cpp/plugin.hpp):
// sync_plugin_context(api) вызывает:
Logger::set_external_logger(
    std::shared_ptr<spdlog::logger>(
        static_cast<spdlog::logger*>(api->internal_logger),
        [](spdlog::logger*) {}  // no-op deleter: ядро владеет объектом
    ));
```

---

## Shutdown

```cpp
// main.cpp, ПОСЛЕ manager.unload_all():
Logger::shutdown();
// 1. inst->flush()
// 2. spdlog::drop_all()
// 3. inst.reset()  → Meyers local = nullptr → безопасно при exit
```

---

## fmt_extensions.hpp

Кастомный `fmt::formatter<R>` для любых range (кроме строк):

```cpp
std::vector<int> v = {1,2,3,4,5};
LOG_DEBUG("Values: {}", v);
// → "Values: int(5): [1, 2, 3, 4, 5]"

std::vector<int> big(100);
LOG_DEBUG("Big: {}", big);
// → "int(100): [0, 1, 2, 3, ..., 97, 98, 99, 0]"
// первые 4 + "..." + последние 4 (MAX_VISIBLE = 8)
```

Константы: `MAX_VISIBLE = 8`, `MULTILINE_THRESHOLD = 15`.

---

*← [08 — Идентификация](08-identity.md) · [10 — Config →](10-config.md)*
