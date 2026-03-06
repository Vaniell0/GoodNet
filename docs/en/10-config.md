# 10 — Config

`include/config.hpp` · `src/config.cpp`

---

## Value Types

```cpp
using Value = std::variant<int, bool, double, std::string, fs::path>;
```

Storage: flat `unordered_map<string, Value>` with hierarchical keys via `.`.

---

## Hierarchical Keys

JSON is recursively flattened:

```json
{ "core": { "listen_port": 25565 }, "logging": { "level": "info" } }
```
```cpp
config.get<int>("core.listen_port");      // 25565
config.get<std::string>("logging.level"); // "info"
```

`to_json()` performs the reverse — reassembles dotted keys into nested JSON.

---

## fs::path vs std::string

Defaults are set via `set<fs::path>()` — stored as `fs::path`.
JSON parsing loads strings as `std::string`. `get<fs::path>()` handles both transparently:

```cpp
if (const auto* p = std::get_if<fs::path>(&it->second)) return *p;
if (const auto* s = std::get_if<std::string>(&it->second)) return fs::path(*s);
```

---

## Defaults

```cpp
"core.listen_address"  = "0.0.0.0"
"core.listen_port"     = 25565
"core.io_threads"      = 4
"core.max_connections" = 1000

"logging.level"     = "info"
"logging.file"      = "logs/goodnet.log"
"logging.max_size"  = 10485760
"logging.max_files" = 5

"plugins.base_dir"   = cwd/plugins
"plugins.auto_load"  = true

"security.key_exchange_timeout" = 30
"security.max_auth_attempts"    = 3
"security.session_timeout"      = 3600
```

---

## API

```cpp
Config cfg;                        // defaults + load config.json from cwd
cfg.load_from_file("my.json");
cfg.save_to_file("out.json");

cfg.set("core.listen_port", 8080);
auto port = cfg.get<int>("core.listen_port");        // optional<int>
auto dir  = cfg.get_or<fs::path>("plugins.base_dir", fs::current_path());

bool exists = cfg.has("logging.level");
cfg.remove("debug.verbose");
```

---

*← [09 — Logger](09-logger.md) · [11 — C SDK →](11-sdk-c.md)*
