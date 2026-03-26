# Конфигурация

Typed struct конфигурация с JSON persistence и прямым доступом к полям.

См. также: [Быстрый старт](./quickstart.md) · [Сборка](./build.md) · [Криптография](./protocol/crypto.md) · **[Config recipes →](./recipes/config-recipes.md)**

## Config API

`Config` (`include/config.hpp`, `src/config.cpp`) — типизированная иерархическая конфигурация. Все настройки — вложенные struct-ы внутри `class Config` с sensible defaults.

```cpp
#include "config.hpp"

Config cfg(true);  // true = только defaults, без попытки загрузить config.json

// Прямой доступ к полям
cfg.core.io_threads = 4;
cfg.core.listen_port = 25565;
cfg.logging.level = "debug";
cfg.compression.enabled = true;
cfg.identity.dir = "~/.goodnet";
cfg.ice.stun_servers = "stun.l.google.com:19302";
```

Если `Config(false)` (или без аргумента) — конструктор попытается загрузить `config.json` из текущего каталога.

### JSON persistence

```cpp
// Загрузка из файла
cfg.load_from_file("config.json");

// Загрузка из строки
cfg.load_from_string(R"({"core": {"io_threads": 8}})");

// Сохранение
cfg.save_to_file("config.json");
std::string json = cfg.save_to_string();

// Перезагрузка из последнего успешно загруженного файла
cfg.reload();
```

**⚠️ Config reload safety:** `cfg.reload()` небезопасен во время `run()` (data race). Config должен быть immutable после `run_async()`. Для dynamic reload: остановить Core → reload → перезапустить.

### Flat key access (CAPI / plugin backward-compat)

Для C API и плагинов доступен read-only доступ по dotted ключам:

```cpp
auto val = cfg.get_raw("core.io_threads");  // → optional<string>("4")
auto lvl = cfg.get_raw("logging.level");    // → optional<string>("info")
auto bad = cfg.get_raw("nonexistent");      // → nullopt
```

`get_raw()` возвращает `std::optional<std::string>` — все значения конвертируются в строки. Полезно для `host_api_t::config_get()` в плагинах.

## JSON формат

Формат — **вложенные объекты** (не плоские dotted ключи):

```json
{
  "core": {
    "listen_address": "0.0.0.0",
    "listen_port": 25565,
    "io_threads": 0,
    "max_connections": 1000
  },
  "logging": {
    "level": "info",
    "file": "",
    "max_size": 10485760,
    "max_files": 5
  },
  "security": {
    "key_exchange_timeout": 30,
    "max_auth_attempts": 3,
    "session_timeout": 3600
  },
  "compression": {
    "enabled": true,
    "threshold": 512,
    "level": 1
  },
  "plugins": {
    "base_dir": "",
    "auto_load": true,
    "scan_interval": 300,
    "extra_dirs": ""
  },
  "identity": {
    "dir": "~/.goodnet",
    "ssh_key_path": "",
    "use_machine_id": true,
    "skip_ssh_fallback": false
  },
  "ice": {
    "stun_servers": "stun.l.google.com:19302,stun1.l.google.com:19302,stun2.l.google.com:19302",
    "session_timeout": 10,
    "keepalive_interval": 20,
    "consent_max_failures": 3
  }
}
```

## Секции

Все секции — вложенные struct-ы внутри `class Config` (`Config::Core`, `Config::Log`, etc.).

### Config::Core

| Поле | Тип | Default | Описание |
|------|-----|---------|----------|
| `listen_address` | string | `"0.0.0.0"` | Адрес для входящих соединений |
| `listen_port` | int | `25565` | Порт для входящих соединений |
| `io_threads` | int | `0` | IO потоки. 0 = `hardware_concurrency` |
| `max_connections` | int | `1000` | Максимум одновременных соединений |

```cpp
cfg.core.io_threads = 4;
cfg.core.listen_port = 8080;
```

### Config::Log

| Поле | Тип | Default | Описание |
|------|-----|---------|----------|
| `level` | string | `"info"` | trace, debug, info, warn, error, critical, off |
| `file` | string | `""` | Путь к лог-файлу. Пусто = только console |
| `max_size` | int | `10485760` | Макс. размер файла в bytes (10 MB) |
| `max_files` | int | `5` | Количество ротируемых файлов |

```cpp
cfg.logging.level = "debug";
cfg.logging.file = "/var/log/goodnet.log";
```

### Config::Security

| Поле | Тип | Default | Описание |
|------|-----|---------|----------|
| `key_exchange_timeout` | int | `30` | Таймаут handshake (секунды) |
| `max_auth_attempts` | int | `3` | Макс. попыток аутентификации |
| `session_timeout` | int | `3600` | Таймаут сессии (секунды) |

```cpp
cfg.security.key_exchange_timeout = 60;
cfg.security.session_timeout = 7200;
```

### Config::Compression

| Поле | Тип | Default | Описание |
|------|-----|---------|----------|
| `enabled` | bool | `true` | Включить zstd сжатие |
| `threshold` | int | `512` | Минимальный размер payload для сжатия (bytes) |
| `level` | int | `1` | Уровень сжатия zstd |

```cpp
cfg.compression.enabled = true;
cfg.compression.threshold = 1024;
cfg.compression.level = 3;
```

### Config::Plugins

| Поле | Тип | Default | Описание |
|------|-----|---------|----------|
| `base_dir` | string | `""` | Базовый каталог плагинов. Пусто = не загружать |
| `auto_load` | bool | `true` | Автозагрузка .so из base_dir при старте |
| `scan_interval` | int | `300` | Интервал сканирования каталога (секунды) |
| `extra_dirs` | string | `""` | Доп. каталоги через `;` |

```cpp
cfg.plugins.base_dir = "/opt/goodnet/plugins";
cfg.plugins.extra_dirs = "/home/user/my_plugins;/opt/extra";
```

Приоритет поиска плагинов:
1. `GOODNET_PLUGINS_DIR` (env var) — высший приоритет
2. `plugins.base_dir` — из конфига
3. `plugins.extra_dirs` — дополнительные каталоги через `;`

### Config::Identity

| Поле | Тип | Default | Описание |
|------|-----|---------|----------|
| `dir` | string | `"~/.goodnet"` | Каталог с ключами. `~` раскрывается |
| `ssh_key_path` | string | `""` | Импорт из OpenSSH Ed25519 |
| `use_machine_id` | bool | `true` | Привязать device key к hardware fingerprint |
| `skip_ssh_fallback` | bool | `false` | Не пробовать `~/.ssh/id_ed25519` автоматически |

```cpp
cfg.identity.dir = "/etc/goodnet/keys";
cfg.identity.ssh_key_path = "~/.ssh/id_ed25519";
```

Подробнее о ключах и identity: [Криптография →](./protocol/crypto.md) · **[Identity migration →](./recipes/identity-migration.md)**

### Config::Ice

| Поле | Тип | Default | Описание |
|------|-----|---------|----------|
| `stun_servers` | string | `"stun.l.google.com:19302,..."` | STUN серверы CSV (`host:port,host:port`) |
| `session_timeout` | int | `10` | Таймаут ICE сессии (секунды) |
| `keepalive_interval` | int | `20` | Интервал keepalive (секунды) |
| `consent_max_failures` | int | `3` | Макс. пропущенных consent проверок |

```cpp
cfg.ice.stun_servers = "stun.l.google.com:19302,stun1.l.google.com:19302";
cfg.ice.session_timeout = 15;
```

ICE плагин парсит CSV строку через `parse_stun_servers()` при инициализации. Env var `GOODNET_STUN_SERVER` используется как fallback.

## Logger

spdlog Meyers singleton (`src/logger.cpp`). Один экземпляр на процесс — разделяется между core и всеми плагинами через SHARED library.

```cpp
LOG_INFO("message {}", value);
LOG_WARN("warning: {}", msg);
LOG_ERROR("error: {}", err);
LOG_DEBUG("debug data: {}", hex);   // zero-cost в Release (NDEBUG)
LOG_TRACE("trace");                 // zero-cost в Release (NDEBUG)
```

Конфигурация Logger задаётся через `cfg.logging.*` при инициализации Core. Custom `%Q` spdlog pattern flag показывает `файл:строка` относительно project root.

Подробности: `include/logger.hpp`.

## Переменные окружения

| Переменная | Описание |
|-----------|----------|
| `GOODNET_PLUGINS_DIR` | Каталог плагинов (приоритет над `plugins.base_dir`) |
| `GOODNET_STUN_SERVER` | STUN сервер для [ICE connector](./guides/connector-guide.md) |
| `GOODNET_STUN_PORT` | STUN порт для ICE |
| `HOME` | Для раскрытия `~` в путях |

---

**См. также:** [Config recipes](./recipes/config-recipes.md) · [Identity migration](./recipes/identity-migration.md) · [Быстрый старт](./quickstart.md) · [Сборка](./build.md)
