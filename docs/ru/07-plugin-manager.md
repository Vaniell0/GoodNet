# 07 — PluginManager

`core/pluginManager.hpp` · `core/pluginManager_core.cpp` · `core/pluginManager_query.cpp`

---

## Обязанности

1. SHA-256 верификация `.so` по JSON-манифесту — **до** `dlopen`
2. `dlopen(RTLD_NOW | RTLD_LOCAL)` через `DynLib`
3. Чтение `plugin_get_info()` — тип плагина, приоритет, capability flags
4. Инициализация + хранение `HandlerInfo` / `ConnectorInfo` с RAII-владением
5. Регистрация хендлеров в `SignalBus` с учётом `plugin_info_t::priority`
6. Управление состоянием: enable / disable / unload без перезапуска ядра

---

## plugin_info_t

```c
// sdk/types.h
typedef struct {
    const char* name;      // уникальное имя плагина
    uint32_t    version;   // (major<<16)|(minor<<8)|patch
    uint8_t     priority;  // приоритет в цепочке: 255 = первый, 0 = последний
    uint8_t     _pad[3];
    uint32_t    caps_mask; // PLUGIN_CAP_* флаги
} plugin_info_t;

#define PLUGIN_CAP_COMPRESS_ZSTD (1U << 0)
#define PLUGIN_CAP_ICE_SUPPORT   (1U << 1)
#define PLUGIN_CAP_HOT_RELOAD    (1U << 2)
```

`plugin_get_info()` — опциональный символ, вызывается **до** `*_init()`. Позволяет PluginManager прочитать метаданные без инициализации плагина (например, для листинга или проверки версии).

---

## plugin_state_t (Hot-reload state machine)

```c
typedef enum {
    PLUGIN_STATE_PREPARING = 0, // Загружен; новый трафик не получает
    PLUGIN_STATE_ACTIVE    = 1, // Основной хендлер для новых соединений
    PLUGIN_STATE_DRAINING  = 2, // Старая версия; обслуживает существующие сессии
    PLUGIN_STATE_ZOMBIE    = 3  // Нет активных соединений; ожидает dlclose
} plugin_state_t;
```

В текущей alpha версии все загруженные плагины сразу переходят в `PLUGIN_STATE_ACTIVE`. Полный hot-reload (PREPARING → ACTIVE → DRAINING → ZOMBIE) запланирован на beta.

---

## Жизненный цикл загрузки

```
load_plugin(path.so)
    │
    ├─ 1. exists(path)?                     ✗ → unexpected("not found")
    │
    ├─ 2. verify_metadata(path)
    │       exists(path + ".json")?         ✗ → unexpected("manifest missing")
    │       json::parse(manifest_file)
    │       expected = json["integrity"]["hash"]
    │       actual   = calculate_sha256(path)
    │       expected == actual?             ✗ → unexpected("hash mismatch")
    │
    ├─ 3. DynLib::open(path, RTLD_NOW|RTLD_LOCAL)
    │                                       ✗ → unexpected(dlerror())
    │
    ├─ 4. plugin_get_info() найден?
    │       info = (*plugin_get_info)()     ← читаем приоритет и caps_mask
    │
    ├─ 5a. symbol("handler_init") найден?
    │       HandlerInfo info;
    │       info.lib   = move(lib);
    │       info.api_c = *host_api_;        ← КОПИЯ, не указатель
    │       info.api_c.plugin_info = &info.static_info;
    │       info.api_c.plugin_type = PLUGIN_TYPE_HANDLER; // deprecated, но заполняем
    │       (*handler_init)(&info.api_c, &info.handler);
    │       // Регистрируем в SignalBus с приоритетом из plugin_info
    │       bus_.subscribe(type, name, cb, info.handler->info->priority);
    │       handlers_[info.handler->name] = move(info);
    │       return {}  (success)
    │
    └─ 5b. symbol("connector_init") найден?
            ConnectorInfo info;
            info.api_c.plugin_type = PLUGIN_TYPE_CONNECTOR;
            (*connector_init)(&info.api_c, &info.ops);
            info.ops->get_scheme(buf, sizeof(buf)) → scheme;
            connectors_[scheme] = move(info);
            return {}  (success)
```

---

## HandlerInfo / ConnectorInfo

```cpp
struct HandlerInfo {
    DynLib       lib;      // RAII: dlclose() в деструкторе
    handler_t*   handler;  // указатель на статический объект внутри .so
    plugin_info_t static_info; // копия данных из plugin_get_info()
    fs::path     path;
    std::string  name;
    bool         enabled = true;
    host_api_t   api_c;   // СОБСТВЕННАЯ копия API

    ~HandlerInfo() {
        if (handler && handler->shutdown)
            handler->shutdown(handler->user_data);
        // ~DynLib() → dlclose() после shutdown()
    }
};
```

**Почему `api_c` — копия, а не указатель?** Плагин хранит `&info.api_c` на всё время жизни. При rehash `unordered_map` указатель стал бы невалидным. Копия в `HandlerInfo` живёт ровно столько, сколько запись в map.

**Порядок деструкции:**
1. `HandlerInfo::~HandlerInfo()` → `handler->shutdown()` → плагин освобождает ресурсы
2. `DynLib::~DynLib()` → `dlclose()` → код `.so` выгружается

---

## Приоритет в цепочке диспетчеризации

`plugin_info_t::priority` (0–255) определяет порядок вызовов в `PipelineSignal`:

```
Priority 255 → вызывается первым (например: security handler, rate limiter)
Priority 128 → дефолт (бизнес-логика)
Priority   0 → вызывается последним (например: wildcard logger)
```

При регистрации хендлера `SignalBus` сортирует `vector<Entry>` по убыванию приоритета. Добавление нового хендлера — O(n) операция под write lock, происходит только при загрузке плагина (не на горячем пути).

---

## SHA-256 верификация

### Формат `<plugin>.so.json`

```json
{
  "meta": {
    "name":    "logger",
    "version": "1.0.0"
  },
  "integrity": {
    "hash": "a3f5c8d2e1b04f..."
  }
}
```

Файл: `liblogger.so` → `liblogger.so.json`. `buildPlugin.nix` генерирует манифест автоматически.

### Потоковый SHA-256

```cpp
static std::string calculate_sha256(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    crypto_hash_sha256_state st;
    crypto_hash_sha256_init(&st);

    char buf[65536]; // 64 KB чанки — файл не грузится целиком
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

## Transparent Hashing

```cpp
struct StringHash {
    using is_transparent = void;
    size_t operator()(std::string_view sv) const noexcept {
        return std::hash<std::string_view>{}(sv);
    }
};

std::unordered_map<std::string, HandlerInfoPtr,
                   StringHash, std::equal_to<>> handlers_;

// Нет аллокации — string_view напрямую в хэш-таблицу:
manager.find_handler_by_name("logger");
```

---

## DynLib

`include/dynlib.hpp` — кроссплатформенный RAII-загрузчик:

| Платформа | Открытие | Символ | Закрытие |
|---|---|---|---|
| Linux/macOS | `dlopen(RTLD_NOW\|RTLD_LOCAL)` | `dlsym` | `dlclose` |
| Windows | `LoadLibraryW` | `GetProcAddress` | `FreeLibrary` |

- `RTLD_LOCAL` — символы изолированы между плагинами (нет конфликта имён)
- `RTLD_NOW` — все символы разрешаются при открытии, ошибка диагностируется сразу

---

## API управления

```cpp
// Загрузка
auto r = manager.load_plugin("plugins/handlers/logger.so");
if (!r) LOG_ERROR("Load failed: {}", r.error());

manager.load_all_plugins(); // сканирует plugins/{handlers,connectors}/

// Поиск
auto h = manager.find_handler_by_name("logger");       // optional<handler_t*>
auto c = manager.find_connector_by_scheme("tcp");      // optional<connector_ops_t*>

// Состояние
manager.enable_handler("debug");
manager.disable_handler("debug");
manager.unload_handler("old");   // shutdown() + dlclose()
manager.unload_all();            // graceful shutdown всего

// Информация
manager.list_plugins();          // LOG_INFO таблицей с приоритетами
size_t n = manager.get_enabled_handler_count();
auto*  info = manager.get_plugin_info("logger"); // plugin_info_t*
```

---

*← [06 — SignalBus](06-signal-bus.md) · [08 — Идентификация →](08-identity.md)*