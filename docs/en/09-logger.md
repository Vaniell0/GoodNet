# 09 — Logger

`include/logger.hpp` · `src/logger.cpp`

---

## Architecture

Meyers Singleton over `spdlog`. Heavy headers (`spdlog/spdlog.h`) are included only in `logger.cpp` — all other translation units get only lightweight forward declarations from `logger.hpp`.

```
Logger (static class)
  ├── get_instance() → static shared_ptr<spdlog::logger>
  ├── ensure_initialized() → std::call_once → init_internal()
  ├── Sinks:
  │   ├── rotating_file_sink_mt  (always)
  │   └── stdout_color_sink_mt   (Debug only, #ifndef NDEBUG)
  ├── Custom flag %Q → source location formatter
  └── shutdown() → flush → drop_all → shared_ptr.reset()
```

---

## Configuration

Set **before the first** `LOG_*`:

```cpp
Logger::log_level          = "debug";
Logger::log_file           = "logs/goodnet.log";
Logger::max_size           = 10 * 1024 * 1024;
Logger::max_files          = 5;
Logger::project_root       = "/home/user/proj/";
Logger::source_detail_mode = 0;  // 0=auto 1=path+line 2=file+line 3=file
```

---

## %Q Flag — Source Location

| mode | trace / debug | info+ |
|---|---|---|
| 0 (auto) | `[src/core/cm_session.cpp:42] ` | `[cm_session.cpp] ` |
| 1 | `[src/core/cm_session.cpp:42] ` | `[src/core/cm_session.cpp:42] ` |
| 2 | `[cm_session.cpp:42] ` | `[cm_session.cpp:42] ` |
| 3 | `[cm_session.cpp] ` | `[cm_session.cpp] ` |

---

## Macros

### Always active (Release + Debug)

```cpp
LOG_INFO("Loaded plugin: {} v{}", name, version);
LOG_WARN("Config key '{}' missing, default: {}", key, def);
LOG_ERROR("Decrypt MAC failed: nonce={}", nonce);
LOG_CRITICAL("libsodium init failed");

INFO_VALUE(conn_count);   // → "conn_count = 5"
INFO_POINTER(session_ptr);
```

### Debug only (#ifndef NDEBUG)

```cpp
LOG_TRACE("Enter: {}", __FUNCTION__);
LOG_DEBUG("recv_buf size={}", buf.size());
TRACE_VALUE(session_key_hex);
LOG_SCOPED_TRACE();   // ">>> func" on entry, "<<< func" on exit
```

---

## Plugin Logger Injection

```cpp
// PluginManager before calling *_init():
info->api_c.internal_logger = Logger::get_raw_ptr();

// In plugin via sync_plugin_context(api):
Logger::set_external_logger(
    std::shared_ptr<spdlog::logger>(
        static_cast<spdlog::logger*>(api->internal_logger),
        [](spdlog::logger*) {}  // no-op deleter: core owns the object
    ));
```

---

## Shutdown

```cpp
// main.cpp, AFTER manager.unload_all():
Logger::shutdown();
// 1. flush  2. spdlog::drop_all()  3. shared_ptr.reset()
// Meyers local sees nullptr at exit → no SIGSEGV
```

---

*← [08 — Identity](08-identity.md) · [10 — Config →](10-config.md)*
