# 07 — Архитектура

## Обзор компонентов

GoodNet построен как монолитное ядро (`goodnet_core.so`) с подключаемыми плагинами. Ядро владеет четырьмя подсистемами:

| Компонент | Файлы | Назначение |
|---|---|---|
| **ConnectionManager** | `core/cm_*.cpp` | Хендшейк, шифрование, TCP-фрейминг, RCU-реестр соединений |
| **PluginManager** | `core/pluginManager_*.cpp` | SHA-256 верификация, dlopen, lifecycle плагинов |
| **SignalBus** | `include/signals.hpp`, `src/signals.cpp` | Маршрутизация пакетов (chain-of-responsibility) и событий |
| **Config** | `include/config.hpp`, `src/config.cpp` | JSON → flat key-value, watchers, persistence |

Вспомогательные:
- **NodeIdentity** (`core/cm_identity.cpp`) — Ed25519 ключи (user + device)
- **Logger** (`include/logger.hpp`, `src/logger.cpp`) — spdlog Meyers singleton
- **DynLib** (`include/dynlib.hpp`) — кроссплатформенный RAII-загрузчик `.so` / `.dll`

---

## Компонентная диаграмма

```
┌──────────────────────────────────────────────────────────────────┐
│                        GoodNet Node                              │
│                                                                  │
│  main.cpp ──▶ gn::Core (Pimpl)                                  │
│                    │                                             │
│  gn_core_create() ─┘  ← C API (capi.cpp)                        │
│                    │                                             │
│  gn::Core::Impl                                                 │
│  ├── io_context + work_guard        Boost.Asio thread pool       │
│  ├── Logger                         spdlog singleton             │
│  ├── Config                         JSON → flat key-value        │
│  │                                                               │
│  ├── NodeIdentity                                                │
│  │   user_key (Ed25519, переносимый / SSH)                       │
│  │   device_key (Ed25519, hardware-bound через MachineId)        │
│  │                                                               │
│  ├── ConnectionManager                                           │
│  │   ├── records_ : conn_id → ConnectionRecord                   │
│  │   │   ├── SessionState (session_key, nonces, ephem_keypair)   │
│  │   │   ├── recv_buf (TCP reassembly)                           │
│  │   │   ├── peer_pubkeys, peer_schemes, peer_core_meta          │
│  │   │   └── affinity_plugin (CONSUMED pin)                      │
│  │   ├── StatsCollector (lock-free atomic counters)              │
│  │   └── SignalBus                                               │
│  │       channels_[msg_type] → PipelineSignal (RCU)              │
│  │       wildcards_          → PipelineSignal                    │
│  │       on_log              → EventSignal (strand)              │
│  │       on_connection_state → EventSignal                       │
│  │                                                               │
│  └── PluginManager                                               │
│      handlers_[name]     → HandlerInfo { DynLib, handler_t* }    │
│      connectors_[scheme] → ConnectorInfo { DynLib, ops_t* }      │
│      SHA-256 verify ДО dlopen(RTLD_NOW|RTLD_LOCAL)               │
└──────────────────────────────────────────────────────────────────┘
```

---

## Pimpl-паттерн (Core → Core::Impl)

`gn::Core` — единственный публичный класс ядра. Весь состояние скрыто за `Core::Impl`:

```cpp
// include/core.hpp — публичный заголовок
class Core {
public:
    explicit Core(CoreConfig cfg = {});
    ~Core();
    // ... публичные методы
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
```

Преимущества:
- **ABI-стабильность** — изменение внутренностей не ломает бинарную совместимость
- **Быстрая компиляция** — пользователь включает только `core.hpp`, не тянет Boost/spdlog
- **Мульти-экземпляр** — синглтон удалён, можно создавать несколько экземпляров Core

---

## ConnectionManager

CM — самый сложный компонент. Разбит на 5 файлов:

| Файл | Назначение |
|---|---|
| `cm_handshake.cpp` | Конструктор CM, `fill_host_api`, AUTH send/receive, KEY_EXCHANGE, ICE init |
| `cm_identity.cpp` | `NodeIdentity::load_or_generate`, парсер OpenSSH Ed25519, `bytes_to_hex` |
| `cm_session.cpp` | `SessionState::encrypt/decrypt` (zstd + XSalsa20-Poly1305), ECDH `derive_session` |
| `cm_dispatch.cpp` | `handle_data` (фрейминг, fast-path zero-copy), `dispatch_packet` (маршрутизация) |
| `cm_send.cpp` | `build_frame`, `send_frame`, `flush_queue`, gather-IO, `send`/`broadcast` |

### ConnectionRecord

Каждое соединение представлено записью в RCU-реестре:

```cpp
struct ConnectionRecord {
    conn_id_t       id;
    conn_state_t    state;           // FSM: CONNECTING → ... → CLOSED
    SessionState    session;         // session_key, nonces, ephemeral keys
    std::vector<uint8_t> recv_buf;   // TCP reassembly buffer
    std::string     peer_uri;
    uint8_t         peer_user_pk[32];
    uint8_t         peer_device_pk[32];
    std::string     peer_schemes;    // "tcp,ice" — транспорты пира
    uint32_t        peer_core_meta;  // capability flags пира
    std::string     affinity_plugin; // хендлер, вернувший CONSUMED
    PerConnQueue    send_queue;      // backpressure: 8 MB лимит
    std::atomic<uint64_t> send_packet_id{0};
};
```

### RCU-реестр

`records_` использует `shared_mutex` с RCU-семантикой:
- **Читатели** (dispatch, send) берут `shared_lock` — параллельный доступ без блокировок
- **Писатели** (handshake, close) берут `unique_lock` — эксклюзивный доступ
- `records_mu_` освобождается **перед** вызовом `SignalBus::dispatch_packet()` — хендлеры не держат lock

---

## SignalBus

Маршрутизирует расшифрованные пакеты от ConnectionManager к хендлер-плагинам. Реализует **chain-of-responsibility**: хендлеры вызываются синхронно по убыванию приоритета.

### Два типа сигналов

**PipelineSignal** — горячий путь (пакетный трафик):

```cpp
class PipelineSignal {
public:
    void connect(uint8_t priority, std::string_view name, HandlerPacketFn fn);
    void disconnect(std::string_view name);

    struct EmitResult {
        propagation_t result = PROPAGATION_CONTINUE;
        std::string   consumed_by;
    };
    EmitResult emit(shared_ptr<header_t>, const endpoint_t*, PacketData) const;
};
```

RCU-семантика: `connect()`/`disconnect()` атомарно подменяют `shared_ptr<const vector<Entry>>`. Читающие потоки `emit()` захватывают snapshot указателя — одна атомарная операция, без блокировок.

**EventSignal** — контрольный путь (логи, изменения состояния):

```cpp
template<typename... Args>
class EventSignal : public EventSignalBase {
public:
    void connect(Handler h);
    void emit(Args... args);  // snapshot + post в strand
};
```

Backpressure: `EventSignalBase` реализует атомарный счётчик задач с лимитом. При `pending_tasks >= MAX_PENDING` событие дропается — защита Asio-очереди от OOM.

### propagation_t

```c
typedef enum {
    PROPAGATION_CONTINUE = 0,  // передать следующему по приоритету
    PROPAGATION_CONSUMED = 1,  // остановить цепочку; пинит session affinity
    PROPAGATION_REJECT   = 2   // дропнуть пакет молча
} propagation_t;
```

### Порядок диспетчеризации

```
dispatch_packet(msg_type, hdr, ep, data)
       │
       ├─ 1. channels_[msg_type] — специфичный канал
       │       handlers отсортированы по priority (255 = первый)
       │       for each handler:
       │         handle_message(...)
       │         r = on_message_result(...)
       │         CONSUMED → return {CONSUMED, name}
       │         REJECT   → return {REJECT, name}
       │
       └─ 2. wildcards_ — если channels_ не CONSUMED/REJECTED
               same logic
```

### Структура SignalBus

```cpp
class SignalBus {
public:
    void subscribe(uint32_t msg_type, std::string_view name,
                   HandlerPacketFn cb, uint8_t prio = 128);
    void subscribe_wildcard(std::string_view name,
                            HandlerPacketFn cb, uint8_t prio = 128);
    PipelineSignal::EmitResult dispatch_packet(
        uint32_t msg_type, shared_ptr<header_t> hdr,
        const endpoint_t* ep, PacketData data);

    EventSignal<std::string>    on_log;
    EventSignal<uint32_t, bool> on_connection_state;
};
```

`PacketData = shared_ptr<vector<uint8_t>>` — данные разделяются между всеми хендлерами без копирования.

---

## PluginManager

### Обязанности

1. SHA-256 верификация `.so` по JSON-манифесту — **до** `dlopen`
2. `dlopen(RTLD_NOW | RTLD_LOCAL)` через `DynLib`
3. Чтение `plugin_get_info()` — тип, приоритет, capability flags
4. Инициализация + хранение `HandlerInfo` / `ConnectorInfo` с RAII
5. Регистрация хендлеров в `SignalBus` с учётом приоритета
6. Управление состоянием: enable / disable / unload без перезапуска

### Жизненный цикл загрузки

```
load_plugin(path.so)
    │
    ├─ 1. exists(path)?                     ✗ → unexpected("not found")
    │
    ├─ 2. verify_metadata(path)
    │       exists(path + ".json")?         ✗ → unexpected("manifest missing")
    │       json::parse(manifest)
    │       expected = json["integrity"]["hash"]
    │       actual   = calculate_sha256(path)
    │       match?                          ✗ → unexpected("hash mismatch")
    │
    ├─ 3. DynLib::open(path, RTLD_NOW|RTLD_LOCAL)
    │
    ├─ 4. plugin_get_info() → читаем приоритет и caps_mask
    │
    ├─ 5a. handler_init найден?
    │       HandlerInfo info;
    │       info.api = *host_api_;            ← КОПИЯ, не указатель
    │       (*handler_init)(&info.api, &info.handler);
    │       bus_.subscribe(type, name, cb, priority);
    │       handlers_[name] = move(info);
    │
    └─ 5b. connector_init найден?
            ConnectorInfo info;
            (*connector_init)(&info.api, &info.ops);
            ops->get_scheme(buf) → scheme;
            connectors_[scheme] = move(info);
```

### plugin_state_t

```c
typedef enum {
    PLUGIN_STATE_PREPARING = 0,  // загружен, трафик не получает
    PLUGIN_STATE_ACTIVE    = 1,  // основной хендлер
    PLUGIN_STATE_DRAINING  = 2,  // старая версия; обслуживает сессии
    PLUGIN_STATE_ZOMBIE    = 3   // ожидает dlclose
} plugin_state_t;
```

В alpha все плагины сразу `ACTIVE`. Полный hot-reload запланирован на beta.

### HandlerInfo и порядок деструкции

```cpp
struct HandlerInfo {
    DynLib        lib;          // RAII: dlclose() в деструкторе
    handler_t*    handler;      // указатель на статический объект в .so
    plugin_info_t static_info;  // копия из plugin_get_info()
    host_api_t    api;          // СОБСТВЕННАЯ копия API
    // ...
};
```

Почему `api` — копия? Плагин хранит `&info.api` на всё время жизни. При rehash `unordered_map` указатель стал бы невалидным.

Порядок деструкции:
1. `handler->shutdown()` — плагин освобождает ресурсы
2. `~DynLib()` → `dlclose()` — код `.so` выгружается

### SHA-256 верификация

Манифест `<plugin>.so.json`:

```json
{
  "meta": { "name": "logger", "version": "1.0.0" },
  "integrity": { "hash": "a3f5c8d2e1b04f..." }
}
```

Потоковый хэш: файл читается чанками по 64 KB через `crypto_hash_sha256_*` (libsodium). `buildPlugin.nix` генерирует манифест автоматически при сборке.

### Transparent Hashing

```cpp
struct StringHash {
    using is_transparent = void;
    size_t operator()(std::string_view sv) const noexcept {
        return std::hash<std::string_view>{}(sv);
    }
};
std::unordered_map<std::string, HandlerInfoPtr, StringHash, std::equal_to<>> handlers_;
```

Поиск по `string_view` без аллокации строки.

---

## Поток входящего пакета

```
TCP/UDP bytes
      │
      ▼  [Connector.so]
api->on_data(conn_id, raw, size)
      │
      ▼  [ConnectionManager::handle_data()]
 fast path: recv_buf пуст && полный фрейм?
   └── ДА → dispatch напрямую (zero-copy)
 slow path: recv_buf += raw
 loop: buf >= sizeof(header_t) + payload_len?
   ├── magic != GNET_MAGIC → clear buf, break
   ├── buf < total         → wait for more
   └── extract full packet
          │
          ▼  [dispatch_packet()]
          ├── MSG_TYPE_AUTH     → process_auth() → ECDH → ESTABLISHED
          ├── MSG_TYPE_KEY_EXCHANGE → rekey_session()
          ├── MSG_TYPE_ICE_SIGNAL  → ICE connector SDP
          └── other:
                state != ESTABLISHED → drop
                is_localhost → plaintext
                else → decrypt (XSalsa20-Poly1305) + decompress (zstd)
                       │
                       ▼  records_mu_ released!
                  SignalBus::dispatch_packet(type, hdr, ep, data)
                       │
                  chain-of-responsibility (по убыванию priority):
                  channels_[type] → wildcards_
                  CONSUMED → стоп + affinity_plugin
                  REJECT   → дроп
```

## Поток исходящего пакета

```
core.send(uri, type, data, size)
      │
      ▼  [ConnectionManager::send()]
 resolve_uri(uri) → conn_id  (uri_index_ lookup)
 state == ESTABLISHED? → иначе: connector->connect()
      │
      ▼  [send_frame(conn_id, type, payload, size)]
 payload > 512 bytes && !is_localhost?
   ├── YES → zstd compress (level 1) + orig_size prefix
   └── NO  → без сжатия
 is_localhost?
   ├── YES → plaintext
   └── NO  → nonce[8] ‖ XSalsa20-Poly1305(payload)
 build header_t: magic, ver, type, len, packet_id, timestamp, sender_id
 frame = header ‖ payload
      │
      ▼  connector_ops_t::send_to(conn_id, frame)
      │  или send_gather() — batch до 64 фреймов через writev()
```

---

## Потокобезопасность

| Операция | Гарантия |
|---|---|
| `PipelineSignal::emit()` | Lock-free (snapshot read) |
| `PipelineSignal::connect/disconnect()` | `unique_lock` при изменении |
| `EventSignal::emit()` | `mutex` → snapshot → async post |
| `SignalBus::dispatch_packet()` | `shared_lock` для канала → lock-free emit |
| `ConnectionManager::handle_data()` | `shared_lock` на records_ |
| `ConnectionManager::send()` | `shared_lock` на records_ |
| Handshake / close | `unique_lock` на records_ |
| `StatsCollector` | Lock-free atomics |
| `PerConnQueue` | Lock-free (fetch_add + rollback) |

---

## Слои зависимостей

```
            ┌──────────────────────────────────┐
            │   main.cpp (CLI binary)          │
            │   boost_program_options          │
            └──────────────┬───────────────────┘
                           │ links
     ┌─────────────────────▼──────────────────────┐
     │          goodnet_core.so                    │
     │                                             │
     │  include/core.hpp  ← публичный C++ API      │
     │  include/core.h    ← публичный C API        │
     │                                             │
     │  Core → CM → SignalBus → PipelineSignal     │
     │       → PluginManager → DynLib              │
     │       → NodeIdentity  → MachineId           │
     │                                             │
     │  Deps: Boost.Asio, libsodium, spdlog,       │
     │        fmt, nlohmann_json, zstd             │
     └─────────────────────┬──────────────────────┘
                           │ dlopen(RTLD_LOCAL)
     ┌─────────────────────▼──────────────────────┐
     │      Plugin.so                              │
     │  handler_init / connector_init              │
     │  sdk/ заголовки только                      │
     └─────────────────────────────────────────────┘
```

Ключевые решения:
- `goodnet_core.so` — SHARED, чтобы Logger singleton и `once_flag` были общими для ядра и плагинов
- `ENABLE_EXPORTS ON` — `-rdynamic`, плагины видят символы ядра
- `RTLD_LOCAL` — символы изолированы между плагинами (нет конфликтов имён)
- `RTLD_NOW` — все символы разрешаются при загрузке, ошибка диагностируется сразу

---

## Структура репозитория

```
GoodNet/
├── cmake/                          # CMake-утилиты
│   ├── GoodNetConfig.cmake.in
│   └── pch.cmake
│
├── core/                           # → часть libgoodnet_core.so
│   ├── connectionManager.hpp
│   ├── cm_identity.cpp             # NodeIdentity, SSH-парсер
│   ├── cm_session.cpp              # Encrypt/decrypt, ECDH
│   ├── cm_handshake.cpp            # AUTH, KEY_EXCHANGE, ICE
│   ├── cm_dispatch.cpp             # Фрейминг, dispatch
│   ├── cm_send.cpp                 # Отправка, gather-IO
│   ├── pluginManager.hpp
│   ├── pluginManager_core.cpp      # load, SHA-256, dlopen
│   ├── pluginManager_query.cpp     # find, enable, disable
│   └── data/
│       ├── machine_id.hpp/cpp      # Hardware fingerprint
│       └── messages.hpp            # Wire payloads (AUTH, HEARTBEAT, RELAY, ICE)
│
├── include/                        # Публичные заголовки
│   ├── core.hpp                    # gn::Core + CoreConfig (Pimpl)
│   ├── core.h                      # C API (gn_core_*)
│   ├── config.hpp                  # JSON config store
│   ├── dynlib.hpp                  # Кроссплатформенный dlopen
│   ├── logger.hpp                  # spdlog Meyers singleton
│   └── signals.hpp                 # SignalBus, PipelineSignal, EventSignal
│
├── sdk/                            # Plugin ABI
│   ├── types.h                     # header_t, conn_state_t, propagation_t
│   ├── plugin.h                    # host_api_t
│   ├── handler.h                   # handler_t
│   ├── connector.h                 # connector_ops_t
│   └── cpp/                        # C++ обёртки для удобства
│       ├── handler.hpp             # IHandler
│       ├── connector.hpp           # IConnector
│       └── data.hpp                # IData, PodData<T>
│
├── plugins/
│   ├── handlers/logger/            # Wildcard message logger
│   └── connectors/
│       ├── tcp/                    # Boost.Asio TCP transport
│       └── ice/                    # libnice ICE/DTLS (NAT traversal)
│
├── src/
│   ├── main.cpp                    # CLI benchmark/server binary
│   ├── core.cpp                    # gn::Core реализация
│   ├── capi.cpp                    # C API обёртка
│   ├── config.cpp                  # Config реализация
│   ├── logger.cpp                  # Logger реализация
│   └── signals.cpp                 # SignalBus реализация
│
├── tests/
│   ├── core.cpp                    # Core lifecycle, subscribe, C API
│   ├── conf.cpp                    # Config тесты
│   ├── plugins.cpp                 # Plugin loading, SHA-256
│   ├── connection_manager.cpp      # CM тесты
│   ├── mock_handler.cpp            # Тестовый хендлер (.so)
│   └── mock_connector.cpp          # Тестовый коннектор (.so)
│
├── store/                          # DB skeleton (не в CMake)
│   ├── store.hpp                   # IStore interface
│   ├── sqlite_store.hpp/cpp        # SQLite реализация
│   └── schema.sql                  # SQL-схема
│
├── nix/                            # Nix build helpers
├── CMakeLists.txt
└── flake.nix
```

---

*← [06 — Методы применения](06-usage-patterns.md) · [08 — Протокол и криптография →](08-protocol.md)*
