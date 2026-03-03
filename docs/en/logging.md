# GoodNet — Logging

## Overview

GoodNet uses a custom `Logger` class built on top of [spdlog](https://github.com/gabime/spdlog). Key properties:

- **Meyers Singleton** — lazy initialization, deterministic destruction
- **Source-aware formatting** — log lines include relative file paths and line numbers
- **Debug/Release split** — `LOG_TRACE` and `LOG_DEBUG` compile to `((void)0)` in Release
- **Plugin bridge** — plugins share the core's logger instance without owning it

---

## The Meyers Singleton

The logger instance is stored as a `static` local variable inside `Logger::get_instance()`:

```cpp
// logger.cpp — the only TU that includes <spdlog/spdlog.h>
std::shared_ptr<spdlog::logger>& Logger::get_instance() noexcept {
    static std::shared_ptr<spdlog::logger> instance;
    return instance;
}
```

**Why this instead of a global `static` member?**

A global static member is initialized at program startup in an order that is undefined across translation units. A static local is initialized **on first call** — safe, lazy, and guaranteed thread-safe by the C++11 standard.

More importantly, it solves the **Static Destruction Order Fiasco** on shutdown:

1. `Logger::shutdown()` calls `get_instance().reset()` → `instance = nullptr`
2. Later, when `libgoodnet_core.so` is unloaded, `__do_global_dtors_aux` calls the destructor of every static local in that library
3. The `shared_ptr` destructor sees `nullptr` → `_M_release` checks `use_count` → skips `_M_dispose`
4. **No SIGSEGV**

Without the explicit `reset()`, step 3 would call `_M_dispose` on an already-destroyed `spdlog::logger` object.

**Header cleanliness:** `<spdlog/spdlog.h>` is included **only in `logger.cpp`**. The public header `logger.hpp` uses only `<spdlog/common.h>` (lightweight) and a forward declaration of `spdlog::logger`. Every translation unit in the project that includes `logger.hpp` avoids pulling in spdlog's heavy template machinery.

---

## Configuration

Logger parameters are public static variables on the `Logger` class. Set them **before** the first `LOG_*` call — the logger initializes lazily on first use.

```cpp
// main.cpp — configure before any LOG_* fires
Logger::log_level   = conf.get_or<std::string>("logging.level", "info");
Logger::log_file    = conf.get_or<std::string>("logging.file",  "logs/goodnet.log");
Logger::max_size    = static_cast<size_t>(conf.get_or<int>("logging.max_size", 10*1024*1024));
Logger::max_files   = conf.get_or<int>("logging.max_files", 5);
```

### `config.json` keys

| Key | Type | Default | Description |
|---|---|---|---|
| `logging.level` | string | `"info"` | `trace` `debug` `info` `warn` `error` `critical` `off` |
| `logging.file` | string | `"logs/goodnet.log"` | Path to the rotating log file |
| `logging.max_size` | int | `10485760` | Max file size in bytes before rotation |
| `logging.max_files` | int | `5` | Number of rotated files to keep |

### Source detail mode

`Logger::source_detail_mode` controls how the source location is printed:

| Mode | Output | When |
|---|---|---|
| `0` (auto) | `[src/main.cpp:42]` for debug/trace, `[main.cpp]` for info+ | default |
| `1` (full) | `[src/main.cpp:42]` | always |
| `2` (medium) | `[main.cpp:42]` | always |
| `3` (minimal) | `[main.cpp]` | always |

`Logger::project_root` trims the absolute path to a relative one. It is set automatically from the `GOODNET_PROJECT_ROOT` CMake define (which expands to `CMAKE_SOURCE_DIR`), so log lines show `[src/config.cpp:42]` instead of the full Nix store path.

---

## Sinks

Two sinks are created at initialization:

| Sink | Condition | Pattern variable |
|---|---|---|
| Rotating file (`logs/goodnet.log`) | Always | `Logger::file_pattern` |
| `stdout` colored console | `#ifndef NDEBUG` (Debug builds only) | `Logger::console_pattern` |

The custom `%Q` flag formatter inserts the source location string (file, optional line) into the pattern. This is implemented entirely inside `logger.cpp` and is invisible to consumers of the header.

---

## Log Macros

### Always-on (info and above)

```cpp
LOG_INFO("Server started on port {}", port);
LOG_WARN("Connection limit approaching: {}/{}", current, max);
LOG_ERROR("Failed to open file: {}", path);
LOG_CRITICAL("Out of memory, aborting");

INFO_VALUE(packet_count);          // expands to: LOG_INFO("packet_count = {}", packet_count)
ERROR_POINTER(handler_ptr);        // LOG_ERROR("handler_ptr [0x... valid:true]", ...)
```

### Debug-only (compiled out in Release)

```cpp
LOG_DEBUG("Processing packet id={}", header->packet_id);
LOG_TRACE("Entering dispatch loop");

DEBUG_VALUE(payload_size);         // LOG_DEBUG("payload_size = {}", payload_size)
TRACE_VALUE_DETAILED(conn_state);  // includes type name and sizeof

TRACE_POINTER(handler_ptr);        // prints address and validity
DEBUG_POINTER(handler_ptr);

SCOPED_TRACE();   // logs ">>> FunctionName" on entry, "<<< FunctionName" on exit
SCOPED_DEBUG();   // same, at debug level
```

All debug macros expand to `((void)0)` when `NDEBUG` is defined (Release builds). There is zero runtime overhead — the compiler eliminates them entirely.

---

## Plugin Logging Bridge

Plugins are loaded with `dlopen(RTLD_LOCAL)`. This means each plugin `.so` has its **own copy** of every static variable, including `Logger::get_instance()`. By default that copy is `nullptr` — the first `LOG_*` call in a plugin would dereference null and crash.

The bridge works through `host_api_t::internal_logger`:

```
┌──────────────────────┐         ┌────────────────────────┐
│   libgoodnet_core.so │         │  liblogger.so (plugin) │
│                      │         │                        │
│  Logger::get()  ─────┼──ptr───►│  api->internal_logger  │
│  (Meyers singleton)  │         │         │              │
│  spdlog::logger obj  │         │  sync_plugin_context() │
└──────────────────────┘         │         │              │
                                 │  shared_ptr<no-op del> │
                                 │         │              │
                                 │  Logger::get_instance()│
                                 │  = (borrowed ptr)      │
                                 └────────────────────────┘
```

In `main.cpp` before plugins are loaded:
```cpp
api.internal_logger = static_cast<void*>(Logger::get().get());
```

In each plugin's entry point (via `HANDLER_PLUGIN` macro):
```cpp
sync_plugin_context(api);
// expands to:
Logger::set_external_logger(
    std::shared_ptr<spdlog::logger>(
        static_cast<spdlog::logger*>(api->internal_logger),
        [](spdlog::logger*) noexcept {}  // no-op deleter
    )
);
```

The plugin's copy of `get_instance()` now points to the core's logger object. The **no-op deleter** ensures that when `dlclose()` destroys the plugin's `shared_ptr`, it does not call `delete` on an object it doesn't own. The core controls the logger's lifetime exclusively through `Logger::shutdown()`.
