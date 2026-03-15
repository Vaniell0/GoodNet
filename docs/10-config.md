# 10 — Конфигурация и Logger

## Config

`include/config.hpp` · `src/config.cpp`

### Типы значений

```cpp
using Value = std::variant<int, bool, double, std::string, fs::path>;
```

Хранилище: плоский `unordered_map<string, Value>` с иерархическими ключами через `.`.

### Иерархические ключи

JSON рекурсивно разворачивается в плоские ключи:

```json
{ "core": { "listen_port": 25565 }, "logging": { "level": "info" } }
```

```cpp
config.get<int>("core.listen_port");       // 25565
config.get<std::string>("logging.level");  // "info"
```

`to_json()` собирает плоские ключи обратно во вложенный JSON.

### Дефолтные значения

```
core.listen_address     = "0.0.0.0"
core.listen_port        = 25565
core.io_threads         = 4
core.max_connections    = 1000

logging.level           = "info"
logging.file            = "logs/goodnet.log"
logging.max_size        = 10485760  (10 MB)
logging.max_files       = 5

plugins.base_dir        = cwd/plugins
plugins.auto_load       = true
plugins.scan_interval   = 300

security.key_exchange_timeout = 30
security.max_auth_attempts    = 3
security.session_timeout      = 3600
```

### ICE конфигурация

```json
{
  "ice.stun_server": "stun.l.google.com",
  "ice.stun_port": "19302"
}
```

Приоритет: JSON config > env vars (`GOODNET_STUN_SERVER`) > default.

### API

```cpp
Config cfg;                             // defaults + config.json из cwd
Config cfg(true);                       // только defaults

cfg.load_from_file("my.json");
cfg.load_from_string(json_str);
cfg.save_to_file("out.json");
std::string s = cfg.save_to_string();

cfg.set("core.listen_port", 8080);
cfg.set("plugins.base_dir", fs::path("/opt/plugins"));

auto port = cfg.get<int>("core.listen_port");           // optional<int>
auto dir  = cfg.get_or<fs::path>("plugins.base_dir",
                                  fs::current_path());  // с дефолтом

bool exists = cfg.has("logging.level");
cfg.remove("debug.verbose");
const auto& all = cfg.all();  // весь map
```

### fs::path vs std::string

`get<fs::path>()` прозрачно обрабатывает оба варианта хранения:

```cpp
template<>
std::optional<fs::path> Config::get<fs::path>(const std::string& key) const {
    auto it = values_.find(key);
    if (it == values_.end()) return std::nullopt;
    if (const auto* p = std::get_if<fs::path>(&it->second)) return *p;
    if (const auto* s = std::get_if<std::string>(&it->second)) return fs::path(*s);
    return std::nullopt;
}
```

### Перегрузка char*

```cpp
// Предотвращает хранение строкового литерала как fs::path:
void set(const std::string& key, const char* value) {
    set<std::string>(key, std::string(value));
}
```

---

## Logger

`include/logger.hpp` · `src/logger.cpp`

### Архитектура

Meyers Singleton поверх spdlog. Тяжёлые заголовки включены только в `logger.cpp`.

```
Logger
  ├── get_instance() → static shared_ptr<spdlog::logger>
  ├── Sinks:
  │   ├── rotating_file_sink_mt  (всегда)
  │   └── stdout_color_sink_mt   (только Debug)
  ├── %Q → custom_source_flag [файл:строка]
  └── shutdown() → flush → drop_all → reset
```

### Конфигурация

Выставляется **до первого** `LOG_*`:

```cpp
Logger::log_level          = "debug";
Logger::log_file           = "logs/goodnet.log";
Logger::max_size           = 10 * 1024 * 1024;
Logger::max_files          = 5;
Logger::project_root       = "/home/user/proj/";
Logger::source_detail_mode = 0;  // 0=авто 1=полный 2=файл+строка 3=файл
```

### Макросы

Всегда активны:

```cpp
LOG_INFO("Loaded plugin: {} v{}", name, version);
LOG_WARN("Config key '{}' missing", key);
LOG_ERROR("Decrypt failed: nonce={}", nonce);
LOG_CRITICAL("libsodium init failed");
INFO_VALUE(var);      // → "var = 5"
INFO_POINTER(ptr);    // → "ptr [0x7f... valid:true]"
```

Только Debug (`#ifndef NDEBUG`):

```cpp
LOG_TRACE("Enter: {}", __FUNCTION__);
LOG_DEBUG("Recv buf size={}", buf.size());
LOG_SCOPED_TRACE();   // ">>> func" / "<<< func"
```

В Release `LOG_TRACE`/`LOG_DEBUG` → `((void)0)`.

### Плагины и Logger

Плагины загружаются с `RTLD_LOCAL` — Logger не просачивается. PluginManager передаёт `internal_logger` через `host_api_t`, плагин вызывает `sync_plugin_context()` для sharing:

```cpp
Logger::set_external_logger(
    std::shared_ptr<spdlog::logger>(
        static_cast<spdlog::logger*>(api->internal_logger),
        [](spdlog::logger*) {}  // no-op deleter
    ));
```

### fmt_extensions.hpp

Форматирование range:

```cpp
std::vector<int> v = {1,2,3,4,5};
LOG_DEBUG("Values: {}", v);
// → "int(5): [1, 2, 3, 4, 5]"
```

Для больших контейнеров: первые 4 + `...` + последние 4 (`MAX_VISIBLE = 8`).

---

*← [09 — Безопасность](09-security.md) · [11 — Тестирование →](11-testing.md)*
