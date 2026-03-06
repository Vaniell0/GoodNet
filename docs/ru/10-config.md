# 10 — Config

`include/config.hpp` · `src/config.cpp`

---

## Типы значений

```cpp
using Value = std::variant<int, bool, double, std::string, fs::path>;
```

Хранилище: плоский `unordered_map<string, Value>` с иерархическими ключами через `.`.

---

## Иерархические ключи

JSON рекурсивно разворачивается в плоские ключи:

```json
{ "core": { "listen_port": 25565 }, "logging": { "level": "info" } }
```
```cpp
config.get<int>("core.listen_port");      // 25565
config.get<std::string>("logging.level"); // "info"
```

`to_json()` выполняет обратную операцию — собирает ключи через `.` обратно в вложенный JSON.

---

## fs::path vs std::string

Дефолтные значения устанавливаются через `set<fs::path>()` — хранятся как `fs::path`.
JSON-парсинг загружает строки как `std::string`. `get<fs::path>()` обрабатывает оба варианта прозрачно:

```cpp
template<>
std::optional<fs::path> Config::get<fs::path>(const std::string& key) const {
    auto it = values_.find(key);
    if (it == values_.end()) return std::nullopt;
    if (const auto* p = std::get_if<fs::path>(&it->second)) return *p;
    if (const auto* s = std::get_if<std::string>(&it->second)) return fs::path(*s);
    LOG_ERROR("Config type mismatch for key '{}'", key);
    return std::nullopt;
}
```

---

## Значения по умолчанию

```cpp
// Core
"core.listen_address"  = "0.0.0.0"
"core.listen_port"     = 25565
"core.io_threads"      = 4
"core.max_connections" = 1000

// Logging
"logging.level"     = "info"
"logging.file"      = "logs/goodnet.log"
"logging.max_size"  = 10485760  (10 MB)
"logging.max_files" = 5

// Plugins
"plugins.base_dir"      = cwd/plugins
"plugins.auto_load"     = true
"plugins.scan_interval" = 300

// Security
"security.key_exchange_timeout" = 30
"security.max_auth_attempts"    = 3
"security.session_timeout"      = 3600
```

---

## API

```cpp
Config cfg;                       // defaults + load config.json из cwd
Config cfg(true);                 // только defaults, без загрузки файла

cfg.load_from_file("my.json");
cfg.load_from_string(json_str);
cfg.save_to_file("out.json");
std::string s = cfg.save_to_string();

cfg.set("core.listen_port", 8080);   // int
cfg.set("plugins.base_dir", fs::path("/opt/plugins"));

auto port = cfg.get<int>("core.listen_port");          // optional<int>
auto dir  = cfg.get_or<fs::path>("plugins.base_dir",
                                  fs::current_path());  // с дефолтом

bool exists = cfg.has("logging.level");
cfg.remove("debug.verbose");
const auto& all = cfg.all();  // весь map для итерации
```

---

## char* → std::string

```cpp
// Перегрузка предотвращает хранение строкового литерала как fs::path:
void set(const std::string& key, const char* value) {
    set<std::string>(key, std::string(value));
}
```

---

*← [09 — Logger](09-logger.md) · [11 — C SDK →](11-sdk-c.md)*
