# Система плагинов

Плагины — единственный способ расширить GoodNet. Два типа: [handler](../guides/handler-guide.md) (обработчик сообщений) и [connector](../guides/connector-guide.md) (транспорт). Реализация: `core/pm/pluginManager.hpp`, `core/pm/core.cpp`, `core/pm/query.cpp`.

См. также: [Handler: гайд](../guides/handler-guide.md) · [Connector: гайд](../guides/connector-guide.md) · [Обзор архитектуры](../architecture.md) · [Сборка](../build.md)

## Типы плагинов

| Тип | Интерфейс | Назначение |
|-----|-----------|-----------|
| Handler | [IHandler](../guides/handler-guide.md) / `handler_t` | Обработка сообщений, бизнес-логика |
| Connector | [IConnector](../guides/connector-guide.md) / `connector_ops_t` | Сетевой транспорт (TCP, ICE, UDP) |

## Сборка

### CMake (динамический .so)

```cmake
add_library(my_handler SHARED my_handler.cpp)
target_link_libraries(my_handler PRIVATE goodnet_core)
# или только SDK headers:
# target_link_libraries(my_handler PRIVATE goodnet_sdk)
```

### SHA-256 manifest

Перед загрузкой PluginManager проверяет SHA-256 hash:

```
my_handler.so        ← сам плагин
my_handler.so.json   ← manifest (рядом с .so)
```

Manifest:
```json
{
  "name": "my_handler",
  "sha256": "a1b2c3d4..."
}
```

Генерация (Nix делает автоматически):
```bash
sha256sum my_handler.so | awk '{print $1}'
```

**SHA-256 verification step-by-step:**

```bash
# Шаг 1: Скомпилировать плагин
g++ -shared -fPIC my_handler.cpp -o libmy_handler.so \
    -I/path/to/goodnet/sdk -std=c++23

# Шаг 2: Вычислить SHA-256 hash
sha256sum libmy_handler.so
# Output:
# a1b2c3d4e5f67890abcdef1234567890abcdef1234567890abcdef1234567890  libmy_handler.so

# Шаг 3: Извлечь только hash (без имени файла)
HASH=$(sha256sum libmy_handler.so | awk '{print $1}')
echo $HASH
# a1b2c3d4e5f67890abcdef1234567890abcdef1234567890abcdef1234567890

# Шаг 4: Создать manifest JSON
cat > libmy_handler.so.json << EOF
{
  "name": "my_handler",
  "sha256": "$HASH"
}
EOF

# Шаг 5: Проверить manifest (красивый вывод)
cat libmy_handler.so.json | jq .
# {
#   "name": "my_handler",
#   "sha256": "a1b2c3d4e5f67890abcdef1234567890abcdef1234567890abcdef1234567890"
# }

# Шаг 6: Скопировать оба файла в plugins dir
cp libmy_handler.so libmy_handler.so.json ~/.goodnet/plugins/
```

**Проверка загрузки:**

```bash
# Запустить Core с DEBUG логами
GOODNET_LOG=debug ./goodnet

# Успешная загрузка:
[DEBUG] PluginManager: Loading libmy_handler.so
[DEBUG]   - Manifest hash: a1b2c3d4...
[DEBUG]   - Computed hash: a1b2c3d4...
[DEBUG]   - Hash match: ✓
[DEBUG]   - dlopen: OK
[DEBUG]   - Registered handler: my_handler (priority=128)

# Неудачная загрузка (hash mismatch):
[ERROR] PluginManager: SHA-256 mismatch for libmy_handler.so
[ERROR]   - Expected: a1b2c3d4...
[ERROR]   - Got:      12345678...
[ERROR]   - Plugin rejected (possible tampering)
```

**Причины hash mismatch:**
- `.so` изменён после генерации manifest (recompile без обновления JSON)
- Tampering (злоумышленник подменил .so, не обновив manifest)
- Filesystem corruption
- Разные компиляторы (GCC vs Clang → разный binary output → разный hash)

**Рекомендация:** При каждом `make` автоматически пересчитывать hash (Nix делает это через derivation).

### Static plugins

Для embedded/single-binary:

```cmake
target_sources(goodnet PRIVATE my_handler.cpp)
target_compile_definitions(goodnet PRIVATE GOODNET_STATIC_PLUGINS)
```

Макрос `HANDLER_PLUGIN(ClassName)` при `GOODNET_STATIC_PLUGINS` генерирует compile-time registration вместо dlsym entry point. Аналогично `CONNECTOR_PLUGIN(ClassName)`.

#### Механизм статической регистрации

Реализация: `sdk/static_registry.hpp`, `sdk/cpp/handler.hpp:215-244`.

```cpp
// sdk/static_registry.hpp
struct StaticPluginEntry {
    const char*      name;
    handler_init_t   handler_init   = nullptr;  // для handler-плагинов
    connector_init_t connector_init = nullptr;  // для connector-плагинов
};

inline std::vector<StaticPluginEntry>& static_plugin_registry() {
    static std::vector<StaticPluginEntry> registry;
    return registry;
}
```

При `GOODNET_STATIC_PLUGINS` макрос `HANDLER_PLUGIN(ClassName)`:
1. Создаёт `static ClassName _gn_plugin_instance`
2. Генерирует `_gn_static_handler_init()` — вызывает `init(api)` + `to_c_handler()`
3. Регистрирует entry в `static_plugin_registry()` через **anonymous namespace struct с конструктором** (выполняется до `main()`)

`PluginManager::load_static_plugins()` итерирует реестр и инициализирует каждый entry идентично динамически загруженному плагину (через `handler_init(api, &result)`).

## Загрузка плагинов

```
load_plugin(path):
  1. Прочитать path.json (SHA-256 manifest)
  2. Вычислить SHA-256 файла .so
  3. manifest_hash == file_hash? → продолжить : отказ
  4. dlopen(path, RTLD_NOW | RTLD_LOCAL)
     RTLD_LOCAL — символы плагина изолированы от других плагинов
  5. dlsym("handler_init") || dlsym("connector_init")
  6. entry_point(host_api, &result)
  7. Зарегистрировать в ConnectionManager
```

Core собирается с `ENABLE_EXPORTS ON` (`-rdynamic`) — плагины видят символы ядра (Logger singleton, типы), но не символы друг друга.

## Plugin lifecycle

```
dlopen → plugin_get_info() → entry_point(host_api, &result)
  │
  ▼
PREPARING → enable() → ACTIVE
  │                       │
  │                       ├─ handle_message() calls
  │                       │
  │                       ├─ disable() → PREPARING
  │                       │
  │                       └─ new version loaded → DRAINING
  │                                                │
  │                                                └─ zero active → ZOMBIE
  │                                                                  │
  └──────────────────────────────────────────────────────────────── unload()
                                                                     │
                                                               shutdown() → dlclose
```

`enable()/disable()` без рестарта процесса.

## Hot-reload pitfalls

### Static globals не сбрасываются

**Проблема:** При `unload() + dlclose()` + новый `load()` статические переменные **не инициализируются заново**:

```cpp
// ❌ WRONG: static state в плагине
static std::unordered_map<conn_id_t, SessionData> sessions;

class MyHandler : public IHandler {
    void handle_message(...) override {
        sessions[ep->conn_id] = {...};  // accumulate data
    }
};
```

**Что происходит:**
```
Время  Операция                   sessions state
─────  ─────────────────────────  ────────────────────────
  t0   load(libmy_handler.so v1)  sessions = {} (empty)
  t1   handle_message(conn=1)     sessions = {1: data}
  t2   handle_message(conn=2)     sessions = {1: ..., 2: ...}
  t3   unload()                   sessions НЕ ОЧИЩЕН!
  t4   dlclose()                  .so выгружен из памяти
  t5   load(libmy_handler.so v2)  sessions = {1: ..., 2: ...}  ← stale data!
       ▲
       │
       Проблема: static initializer НЕ вызывается повторно!
```

**Почему:** `dlclose()` не гарантирует полное освобождение памяти плагина:
- Shared libraries могут быть кэшированы динамическим линкером
- Static storage duration переменные инициализируются только первый раз
- Deinitialization (деструкторы static vars) может не вызываться при dlclose()

**Решение 1: Explicit cleanup в on_shutdown()**

```cpp
// ✅ CORRECT: очистка в on_shutdown()
static std::unordered_map<conn_id_t, SessionData> sessions;

class MyHandler : public IHandler {
    void on_shutdown() override {
        sessions.clear();  // Явная очистка
        LOG_DEBUG("Cleared {} sessions", sessions.size());
    }
};
```

**Решение 2: Хранить state в Config или ConnectionRecord**

```cpp
// ✅ BETTER: state вне плагина
class MyHandler : public IHandler {
    // НЕТ static state

    void handle_message(const header_t* hdr, const endpoint_t* ep, ...) override {
        // Получить state из ядра
        auto* conn_data = get_connection_user_data(ep->conn_id);
        // work with conn_data
    }
};
```

**Решение 3: Singleton wrapper с reset()**

```cpp
// ✅ GOOD: controllable singleton
class SessionManager {
    static SessionManager& instance() {
        static SessionManager mgr;
        return mgr;
    }

    void reset() {
        sessions_.clear();
        LOG_INFO("SessionManager reset");
    }

    std::unordered_map<conn_id_t, SessionData> sessions_;
};

class MyHandler : public IHandler {
    void on_shutdown() override {
        SessionManager::instance().reset();  // Явный reset
    }
};
```

### Shared state между версиями плагина

**Проблема:** Если v1 и v2 плагина одновременно загружены (во время graceful migration), они **share static globals**:

```
Время  Операция                     Static var state
─────  ───────────────────────────  ───────────────────────────
  t0   load(v1)                     counter = 0
  t1   v1: handle_message()         counter = 1
  t2   load(v2) (graceful reload)   counter = 1  ← v2 видит v1 state!
  t3   v2: handle_message()         counter = 2  ← race condition!
  t4   v1: handle_message()         counter = 3  ← race!
```

**Защита:** Mutex или atomic для shared static state, но лучше **избегать static globals вообще**.

### Logger singleton gotcha

Logger (`gn::Logger`) — singleton в `libgoodn_core.so`. Плагин может безопасно использовать `LOG_*` макросы, даже при reload — указатель на singleton остаётся валидным (Core `.so` не выгружается).

**НО:** Если плагин создаёт свой `spdlog::logger` (не через `gn::Logger::get()`), он будет unique per-instance → при reload v2 создаст новый logger → duplicate output.

## Enable / Disable API

Handlers можно включать и отключать без выгрузки плагина (`core/pm/pluginManager.hpp`):

```cpp
bool enable_handler (std::string_view name);  // включить отключённый handler
bool disable_handler(std::string_view name);  // отключить (останавливает dispatch, не выгружает)
```

**Отличие от unload:** `disable_handler()` сохраняет плагин в памяти (dlopen handle остаётся открытым), но исключает его из dispatch chain. `enable_handler()` возвращает его обратно. Это позволяет быстро переключать обработчики без накладных расходов на `dlopen/dlclose`.

Lifecycle при disable:
```
ACTIVE → disable_handler("my_handler") → PREPARING
PREPARING → enable_handler("my_handler") → ACTIVE
```

## Query API

`PluginManager` предоставляет набор query-методов для инспекции загруженных плагинов:

```cpp
// Поиск по имени/схеме
std::optional<handler_t*>       find_handler_by_name    (std::string_view name)   const;
std::optional<connector_ops_t*> find_connector_by_scheme(std::string_view scheme) const;

// Списки активных (enabled) плагинов
std::vector<handler_t*>       get_active_handlers()       const;
std::vector<connector_ops_t*> get_active_connectors()     const;
std::vector<std::string>      get_enabled_handler_names() const;

// Счётчики
size_t get_enabled_handler_count()   const;
size_t get_enabled_connector_count() const;
```

**`find_handler_by_name()`** — возвращает `handler_t*` если handler найден **и** enabled, `nullopt` иначе.

**`find_connector_by_scheme()`** — аналогично для connector по URI-схеме (напр. `"tcp"`, `"ice"`).

**`get_enabled_handler_names()`** — список имён всех включённых handlers (для диагностики и CAPI).

**`get_enabled_handler_count()` / `get_enabled_connector_count()`** — быстрые счётчики, доступны также через CAPI: `gn_core_handler_count()`, `gn_core_connector_count()`.

Все query-методы thread-safe (internal shared_mutex).

## C ABI (для не-C++ плагинов)

Если плагин написан на C, Rust, или другом языке — реализуйте `handler_init` / `connector_init` напрямую:

```c
// C handler
#include <sdk/handler.h>

static handler_t my_handler;

static void my_handle(void* ud, const header_t* hdr,
                      const endpoint_t* ep,
                      const void* data, size_t size) {
    // ...
}

int handler_init(host_api_t* api, handler_t** out) {
    my_handler.name = "my_c_handler";
    my_handler.handle_message = my_handle;
    my_handler.user_data = NULL;
    *out = &my_handler;
    return 0;
}
```

SDK C ABI headers: `sdk/types.h`, `sdk/plugin.h`, `sdk/handler.h`, `sdk/connector.h`.

## Шаблоны

Готовые скелеты:
- `plugins/handlers/template/` — handler
- `plugins/connectors/template/` — connector

## Поиск плагинов

Плагины ищутся в следующем порядке приоритета:
1. `GOODNET_PLUGINS_DIR` (env var) — высший
2. `plugins.base_dir` — из [конфига](../config.md#pluginsconfig)
3. `plugins.extra_dirs` — дополнительные каталоги через `;`

---

**См. также:** [Handler: гайд](../guides/handler-guide.md) · [Connector: гайд](../guides/connector-guide.md) · [Обзор архитектуры](../architecture.md) · [Сборка](../build.md) · [Конфигурация](../config.md)
