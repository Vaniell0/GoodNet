# 07 — PluginManager

`core/pluginManager.hpp` · `core/pluginManager_core.cpp` · `core/pluginManager_query.cpp`

---

## Обязанности

1. SHA-256 верификация `.so` по JSON-манифесту — **до** `dlopen`
2. `dlopen(RTLD_NOW | RTLD_LOCAL)` через `DynLib`
3. Определение типа плагина: `handler_init` или `connector_init`
4. Инициализация + хранение `HandlerInfo` / `ConnectorInfo` с RAII-владением
5. Управление состоянием: enable / disable / unload без перезапуска ядра

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
    │       actual   = calculate_sha256(path)   ← потоковый, 64 KB буфер
    │       expected == actual?             ✗ → unexpected("hash mismatch")
    │
    ├─ 3. DynLib::open(path, RTLD_NOW|RTLD_LOCAL)
    │                                       ✗ → unexpected(dlerror())
    │
    ├─ 4a. symbol("handler_init") найден?
    │       HandlerInfo info;
    │       info.lib   = move(lib);
    │       info.api_c = *host_api_;            ← КОПИЯ, не указатель
    │       info.api_c.plugin_type = PLUGIN_TYPE_HANDLER;
    │       (*handler_init)(&info.api_c, &info.handler);
    │       handlers_[info.handler->name] = move(info);
    │       return {}  (success)
    │
    └─ 4b. symbol("connector_init") найден?
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
    DynLib      lib;      // RAII: dlclose() в деструкторе
    handler_t*  handler;  // указатель на статический объект внутри .so
    fs::path    path;
    std::string name;
    bool        enabled = true;
    host_api_t  api_c;    // СОБСТВЕННАЯ копия API

    ~HandlerInfo() {
        // Вызываем shutdown() ДО dlclose()
        if (handler && handler->shutdown)
            handler->shutdown(handler->user_data);
        // ~DynLib() вызовется после — dlclose() безопасен
    }
};
```

**Почему `api_c` — копия, а не указатель?**

Плагин получает `&info.api_c` и хранит его на всё время жизни. Если бы использовался указатель на внешний объект, любой rehash `unordered_map` сделал бы этот указатель невалидным. Копия в `HandlerInfo` живёт ровно столько, сколько запись в map.

**Порядок деструкции:**
1. `HandlerInfo::~HandlerInfo()` → `handler->shutdown()` → плагин освобождает ресурсы
2. `DynLib::~DynLib()` → `dlclose()` → код `.so` выгружается

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

Файл: `liblogger.so` → `liblogger.so.json` (`.json` добавляется к полному имени).

`buildPlugin.nix` генерирует манифест автоматически при каждой сборке Nix.

### Потоковый SHA-256

```cpp
static std::string calculate_sha256(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    crypto_hash_sha256_state st;
    crypto_hash_sha256_init(&st);

    char buf[65536]; // 64 KB — файл не грузится целиком в память
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

Поиск по `string_view` без аллокации `std::string`:

```cpp
struct StringHash {
    using is_transparent = void;  // разрешает гетерогенный поиск
    size_t operator()(std::string_view sv) const noexcept {
        return std::hash<std::string_view>{}(sv);
    }
};

std::unordered_map<std::string, HandlerInfoPtr,
                   StringHash, std::equal_to<>> handlers_;

// Нет аллокации — string_view напрямую в хэш-таблицу:
manager.find_handler_by_name("logger");
manager.find_connector_by_scheme(scheme_sv);
```

---

## DynLib

`include/dynlib.hpp` — кроссплатформенный RAII-загрузчик:

| Платформа | Открытие | Символ | Закрытие |
|---|---|---|---|
| Linux/macOS | `dlopen(RTLD_NOW\|RTLD_LOCAL)` | `dlsym` | `dlclose` |
| Windows | `LoadLibraryW` | `GetProcAddress` | `FreeLibrary` |

- `RTLD_LOCAL` — символы изолированы между плагинами (нет конфликта имён)
- `RTLD_NOW` — все символы разрешаются при открытии, ошибка сразу диагностируется

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
manager.enable_handler("debug");    // включить без перезагрузки
manager.disable_handler("debug");   // выключить (SignalBus перестаёт доставлять)
manager.unload_handler("old");      // shutdown() + dlclose()
manager.unload_all();               // graceful shutdown всего

// Информация
manager.list_plugins();             // LOG_INFO таблицей
size_t n = manager.get_enabled_handler_count();
```

---

*← [06 — SignalBus](06-signal-bus.md) · [08 — Идентификация →](08-identity.md)*
