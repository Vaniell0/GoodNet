# 07 — PluginManager

`core/pluginManager.hpp` · `core/pluginManager_core.cpp` · `core/pluginManager_query.cpp`

---

## Responsibilities

1. SHA-256 verification of `.so` against JSON manifest — **before** `dlopen`
2. `dlopen(RTLD_NOW | RTLD_LOCAL)` via `DynLib`
3. Determine plugin type: `handler_init` or `connector_init`
4. Initialize + store `HandlerInfo` / `ConnectorInfo` with RAII ownership
5. State management: enable / disable / unload without core restart

---

## Loading Lifecycle

```
load_plugin(path.so)
    │
    ├─ 1. exists(path)?                     ✗ → unexpected("not found")
    │
    ├─ 2. verify_metadata(path)
    │       exists(path + ".json")?         ✗ → unexpected("manifest missing")
    │       expected = json["integrity"]["hash"]
    │       actual   = calculate_sha256(path)   ← streaming, 64 KB buffer
    │       expected == actual?             ✗ → unexpected("hash mismatch")
    │
    ├─ 3. DynLib::open(path, RTLD_NOW|RTLD_LOCAL)
    │                                       ✗ → unexpected(dlerror())
    │
    ├─ 4a. symbol("handler_init") found?
    │       info.api = *host_api_;            ← COPY, not pointer
    │       info.api.plugin_type = PLUGIN_TYPE_HANDLER;
    │       (*handler_init)(&info.api, &info.handler);
    │       handlers_[info.handler->name] = move(info);
    │       return {}  (success)
    │
    └─ 4b. symbol("connector_init") found?
            info.api.plugin_type = PLUGIN_TYPE_CONNECTOR;
            (*connector_init)(&info.api, &info.ops);
            connectors_[scheme] = move(info);
            return {}
```

---

## HandlerInfo

```cpp
struct HandlerInfo {
    DynLib      lib;      // RAII: dlclose() in destructor
    handler_t*  handler;  // static object inside .so
    bool        enabled = true;
    host_api_t  api;    // OWN COPY of API (not a pointer!)

    ~HandlerInfo() {
        if (handler && handler->shutdown)
            handler->shutdown(handler->user_data); // before dlclose!
    }
};
```

**Why `api` is a copy?** The plugin stores `host_api_t*` for its lifetime. A pointer into an `unordered_map` element is invalidated on rehash. A copy in `HandlerInfo` lives exactly as long as the map entry.

---

## SHA-256 Verification

### Manifest format

```json
{
  "meta":      { "name": "logger", "version": "1.0.0" },
  "integrity": { "hash": "a3f5c8d2e1b04f..." }
}
```

Named `liblogger.so` → `liblogger.so.json` (`.json` appended to full `.so` name).

### Streaming SHA-256

```cpp
static std::string calculate_sha256(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    crypto_hash_sha256_state st;
    crypto_hash_sha256_init(&st);
    char buf[65536];  // 64 KB — file not loaded into memory at once
    while (file.read(buf, sizeof(buf)) || file.gcount() > 0) {
        crypto_hash_sha256_update(&st,
            reinterpret_cast<const uint8_t*>(buf),
            static_cast<unsigned long long>(file.gcount()));
        if (file.eof()) break;
    }
    uint8_t out[32];
    crypto_hash_sha256_final(&st, out);
    return bytes_to_hex(out, 32);
}
```

---

## DynLib

`include/dynlib.hpp` — cross-platform RAII loader:

| Platform | Open | Symbol | Close |
|---|---|---|---|
| Linux/macOS | `dlopen(RTLD_NOW\|RTLD_LOCAL)` | `dlsym` | `dlclose` |
| Windows | `LoadLibraryW` | `GetProcAddress` | `FreeLibrary` |

`RTLD_LOCAL` — plugin symbols are isolated (no name conflicts between plugins).
`RTLD_NOW` — all symbols resolved on open, errors diagnosed immediately.

---

## API

```cpp
auto r = manager.load_plugin("plugins/handlers/logger.so");
manager.load_all_plugins();

auto h = manager.find_handler_by_name("logger");
auto c = manager.find_connector_by_scheme("tcp");

manager.enable_handler("debug");
manager.disable_handler("debug");
manager.unload_handler("old");
manager.unload_all();
```

---

*← [06 — SignalBus](06-signal-bus.md) · [08 — Identity →](08-identity.md)*
