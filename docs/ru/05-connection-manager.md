# 05 — ConnectionManager

`core/connectionManager.hpp` · `core/cm_*.cpp`

Центральный класс. Управляет полным жизненным циклом соединений: от TCP accept до доставки расшифрованного payload в SignalBus с цепочкой ответственности хендлеров.

---

## FSM состояний

```
on_connect() вызван коннектором
        │
        ▼
 ┌──────────────┐   AUTH + ECDH OK
 │ AUTH_PENDING │ ─────────────────────▶ ┌─────────────┐
 └──────────────┘                         │ ESTABLISHED │ ◀─ весь трафик
        │                                 └──────┬──────┘
        │ on_disconnect()                        │ on_disconnect()
        ▼                                        ▼
   ┌─────────┐                              ┌─────────┐
   │ CLOSED  │                              │ CLOSED  │
   └─────────┘                              └─────────┘
```

---

## StatsCollector

Lock-free счётчики производительности. Считываются CLI-командой `stats` и тестами.

```cpp
struct StatsCollector {
    // Трафик
    std::atomic<uint64_t> rx_bytes    {0};
    std::atomic<uint64_t> tx_bytes    {0};
    std::atomic<uint64_t> rx_packets  {0};
    std::atomic<uint64_t> tx_packets  {0};

    // Handshake
    std::atomic<uint64_t> auth_ok     {0};
    std::atomic<uint64_t> auth_fail   {0};
    std::atomic<uint64_t> decrypt_fail{0};

    // Backpressure
    std::atomic<uint64_t> backpressure  {0};  // дропы из-за MAX_IN_FLIGHT

    // Цепочка ответственности
    std::atomic<uint64_t> consumed    {0};  // PROPAGATION_CONSUMED счётчик
    std::atomic<uint64_t> rejected    {0};  // PROPAGATION_REJECT счётчик

    // Соединения
    std::atomic<uint32_t> connections {0};  // текущие
    std::atomic<uint32_t> total_conn  {0};  // всего установлено
    std::atomic<uint32_t> total_disc  {0};  // всего закрыто
};
```

Доступ: `cm.stats().rx_bytes.load()` или `core.cm().stats()`.

---

## ConnectionRecord

```cpp
struct ConnectionRecord {
    conn_id_t    id;
    conn_state_t state       = STATE_AUTH_PENDING;
    endpoint_t   remote;
    std::string  local_scheme;

    std::vector<std::string> peer_schemes;
    std::string              negotiated_scheme;

    uint8_t peer_user_pubkey  [32]{};
    uint8_t peer_device_pubkey[32]{};
    bool    peer_authenticated = false;
    bool    is_localhost       = false;

    gn::msg::CoreMeta peer_core_meta{};      // возможности пира из AUTH CoreMeta
    std::string affinity_plugin;       // имя плагина, пинированного первым CONSUMED

    std::atomic<uint64_t> send_packet_id{0}; // монотонный счётчик пакетов

    std::unique_ptr<SessionState> session;
    std::vector<uint8_t>          recv_buf;   // TCP reassembly
};
```

### Session affinity

После первого `PROPAGATION_CONSUMED` в цепочке dispatch имя хендлера сохраняется в `affinity_plugin` и логируется. Последующие пакеты этого соединения по-прежнему проходят полную цепочку — affinity сейчас только диагностика и счётчик `stats_.consumed`. В beta: hint для оптимизации порядка.

---

## HandlerEntry

```cpp
struct HandlerEntry {
    std::string           name;
    handler_t*            handler  = nullptr;
    uint8_t               priority = 128;
    std::vector<uint32_t> subscribed_types;
};
```

Хранится в `handler_entries_[name]`. Заполняется в `register_handler()` после вызова `bus_.subscribe()`.

---

## Индексы и мьютексы

```cpp
// Соединения
mutable std::shared_mutex records_mu_;
std::unordered_map<conn_id_t, ConnectionRecord> records_;
std::atomic<conn_id_t> next_id_{1};

// Коннекторы
mutable std::shared_mutex connectors_mu_;
std::unordered_map<std::string, connector_ops_t*> connectors_;

// Хендлеры (для диагностики, не для dispatch)
mutable std::shared_mutex handlers_mu_;
std::unordered_map<std::string, HandlerEntry> handler_entries_;

// URI → conn_id
std::unordered_map<std::string, conn_id_t> uri_index_;
mutable std::shared_mutex uri_mu_;

// hex(user_pubkey) → conn_id
std::unordered_map<std::string, conn_id_t> pk_index_;
mutable std::shared_mutex pk_mu_;

// Ключи идентификации (ротируются без перезапуска)
mutable std::shared_mutex identity_mu_;
NodeIdentity identity_;
```

`identity_mu_` — отдельный мьютекс от `records_mu_`. `send_auth()` держит `shared_lock` на оба в правильном порядке. `rotate_identity_keys()` берёт `unique_lock` только на `identity_mu_` — не прерывает обработку пакетов.

---

## Backpressure

```cpp
std::atomic<size_t> pending_bytes_{0};
static constexpr size_t MAX_IN_FLIGHT_BYTES = 512UL * 1024 * 1024;  // 512 МБ
static constexpr size_t CHUNK_SIZE          = 1UL  * 1024 * 1024;   // 1 МБ чанк
```

Логика в `send_on_conn()`:

```cpp
if (pending_bytes_.load() + size > MAX_IN_FLIGHT_BYTES) {
    LOG_WARN("Backpressure: queue full, dropping");
    stats_.backpressure.fetch_add(1, std::memory_order_relaxed);
    return;
}
pending_bytes_.fetch_add(size);

if (size > CHUNK_SIZE * 2) {
    // Большие буферы режем на 1 МБ чанки
    while (offset < size) {
        size_t chunk = std::min(CHUNK_SIZE, size - offset);
        send_frame(conn_id, msg_type, ptr + offset, chunk);
        offset += chunk;
    }
} else {
    send_frame(conn_id, msg_type, payload, size);
}
pending_bytes_.fetch_sub(size);
```

`get_pending_bytes()` доступен для мониторинга через CLI.

---

## Публичный API

```cpp
// Регистрация
void register_connector(const std::string& scheme, connector_ops_t* ops);
void register_handler(handler_t* h);

// Отправка
void send(const char* uri, uint32_t msg_type, const void* payload, size_t size);
void send_on_conn(conn_id_t id, uint32_t msg_type,                   // прямой ответ
                  const void* payload, size_t size);

// Идентификация (thread-safe, без рестарта)
void rotate_identity_keys(const IdentityConfig& cfg);
static gn::msg::CoreMeta local_core_meta();

// Поиск
conn_id_t find_conn_by_pubkey(const char* pubkey_hex) const;         // 64-char hex
std::optional<conn_state_t> get_state(conn_id_t id) const;
std::optional<std::string>  get_negotiated_scheme(conn_id_t id) const;
std::vector<std::string>    get_active_uris() const;
size_t                      connection_count() const;

// Мониторинг
size_t get_pending_bytes() const;
const StatsCollector& stats() const;
StatsCollector&       stats();

// Настройка
void set_scheme_priority(std::vector<std::string> p);
void fill_host_api(host_api_t* api);
void shutdown();
```

### rotate_identity_keys()

```cpp
// cm_handshake.cpp
void ConnectionManager::rotate_identity_keys(const IdentityConfig& cfg) {
    NodeIdentity next = NodeIdentity::load_or_generate(cfg);
    {
        std::unique_lock lk(identity_mu_);
        identity_ = std::move(next);
    }
    LOG_INFO("Identity keys rotated — new user_pk={}",
             identity_.user_pubkey_hex().substr(0, 12));
}
```

Существующие сессии не прерываются — они используют уже установленный `session_key`. Новые соединения (после ротации) получат новые ключи в AUTH.

### send_on_conn() vs send()

```cpp
// send() — поиск conn_id по URI, потом вызов send_on_conn:
void send(const char* uri, uint32_t type, const void* payload, size_t size) {
    auto id = resolve_uri(uri);
    if (!id) { /* initiate connect */ return; }
    send_on_conn(*id, type, payload, size);
}

// send_on_conn() — прямая отправка по conn_id (без URI lookup):
// Вызывается из host_api_t::send_response через s_send_response
```

---

## Thread safety

```
records_mu_     shared_lock / unique_lock
connectors_mu_  shared_lock / unique_lock
handlers_mu_    shared_lock / unique_lock
uri_mu_         shared_lock / unique_lock
pk_mu_          shared_lock / unique_lock
identity_mu_    shared_lock (send_auth, send_frame) / unique_lock (rotate)
```

**Критический момент в `dispatch_packet`:**

```cpp
// records_mu_ held during decrypt:
{
    std::unique_lock lk(records_mu_);
    // ... decrypt ...
    plaintext = sess.decrypt(...);
    affinity_hint = rec.affinity_plugin;
}
// ← lk освобождён

// dispatch вызывается БЕЗ records_mu_:
auto result = bus_.dispatch_packet(type, hdr_ptr, &remote, data_ptr);
// Хендлер может вызвать api->send_response() → send_on_conn() → records_mu_
// Без unlock была бы deadlock

// Возврат в records_mu_ для записи affinity:
if (result.result == PROPAGATION_CONSUMED && affinity_hint.empty()) {
    std::unique_lock lk(records_mu_);
    records_[id].affinity_plugin = result.consumed_by;
}
```

---

## TCP Reassembly + Fast Path

```cpp
// cm_dispatch.cpp: handle_data()

// Fast path: полный фрейм, recv_buf пуст — zero-copy dispatch.
if (rec->recv_buf.empty() && size >= sizeof(header_t)) {
    const auto* hdr = reinterpret_cast<const header_t*>(raw);
    const size_t total = sizeof(header_t) + hdr->payload_len;
    if (size == total && hdr->magic == GNET_MAGIC
        && hdr->proto_ver == GNET_PROTO_VER) {
        dispatch_packet(id, hdr, payload, recv_ts);
        return;
    }
}

// Slow path: частичные данные или несколько фреймов
rec->recv_buf.insert(rec->recv_buf.end(), data, data + size);
// ... цикл reassembly без изменений ...
```

TCP-коннектор отправляет ровно один полный фрейм за вызов `notify_data()`. Когда `recv_buf` пуст (нет остатков от предыдущих вызовов), fast path диспетчеризирует напрямую из входящего указателя — минуя все копирования буферов.

---

## Разбивка по файлам

| Файл | Что внутри |
|---|---|
| `cm_identity.cpp` | `NodeIdentity::load_or_generate`, OpenSSH Ed25519 parser |
| `cm_session.cpp` | `SessionState::encrypt/decrypt` (XSalsa20-Poly1305 + Zstd), `derive_session` |
| `cm_handshake.cpp` | Конструктор CM, `fill_host_api`, `register_*`, `handle_connect`, `send_auth`, `process_auth`, `handle_disconnect`, `rotate_identity_keys`, `local_core_meta`, C-ABI статики |
| `cm_dispatch.cpp` | `handle_data` (TCP reassembly), `dispatch_packet`, `send_on_conn` |
| `cm_send.cpp` | `send`, `send_on_conn`, `send_frame`, `negotiate_scheme`, `resolve_uri`, геттеры |

---

*← [04 — Криптография](04-crypto.md) · [06 — SignalBus →](06-signal-bus.md)*
