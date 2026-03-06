# 05 — ConnectionManager

`core/connectionManager.hpp` · `core/cm_*.cpp`

Центральный класс. Управляет полным жизненным циклом соединений от TCP accept до доставки расшифрованного payload в SignalBus.

---

## FSM состояний

```
on_connect() вызван коннектором
        │
        ▼
 ┌──────────────┐     AUTH + ECDH OK
 │ AUTH_PENDING │ ──────────────────────▶ ┌─────────────┐
 └──────────────┘                          │ ESTABLISHED │ ◀─ весь трафик
        │                                  └──────┬──────┘
        │ on_disconnect()                         │ on_disconnect()
        ▼                                         ▼
   ┌─────────┐                               ┌─────────┐
   │ CLOSED  │                               │ CLOSED  │
   └─────────┘                               └─────────┘

STATE_CONNECTING   — устанавливается коннектором до вызова api->on_connect()
STATE_KEY_EXCHANGE — зарезервировано (не используется)
STATE_CLOSING      — graceful close (TODO beta)
STATE_BLOCKED      — заблокировано политикой (TODO)
```

---

## ConnectionRecord

```cpp
struct ConnectionRecord {
    conn_id_t    id;
    conn_state_t state = STATE_AUTH_PENDING;

    endpoint_t   remote;               // IP:port + peer_pubkey (после AUTH)
    std::string  local_scheme;         // схема, по которой пришло соединение
    std::string  negotiated_scheme;    // выбранная после capability negotiation

    std::vector<std::string> peer_schemes;   // объявлено пиром в AUTH

    uint8_t peer_user_pubkey  [32]{};
    uint8_t peer_device_pubkey[32]{};
    bool    peer_authenticated = false;
    bool    is_localhost       = false;   // пропускать крипто

    std::unique_ptr<SessionState> session; // nullptr до ECDH
    std::vector<uint8_t>          recv_buf; // TCP reassembly buffer
};
```

---

## Индексы

```cpp
// Основное хранилище
std::unordered_map<conn_id_t, ConnectionRecord> records_;
mutable std::shared_mutex records_mu_;

// URI → conn_id  (для send(uri, ...))
std::unordered_map<std::string, conn_id_t> uri_index_;
mutable std::shared_mutex uri_mu_;

// hex(user_pubkey) → conn_id  (для поиска пира по ключу)
std::unordered_map<std::string, conn_id_t> pk_index_;
mutable std::shared_mutex pk_mu_;
```

Три отдельных mutex — чтение из `uri_index_` не блокирует запись в `records_`.

---

## Thread safety

```
records_mu_    → shared_lock для read, unique_lock для write
uri_mu_        → shared_lock / unique_lock
pk_mu_         → shared_lock / unique_lock
```

**Критический момент в `dispatch_packet`:**

```cpp
// records_mu_ держится во время decrypt:
std::vector<uint8_t> plain;
{
    std::unique_lock lk(records_mu_);
    // ... decrypt ...
    plain = sess.decrypt(cipher, len);
}
// ← lk освобождён

// emit вызывается БЕЗ records_mu_:
bus_.emit(type, hdr_ptr, &remote, make_shared<vector>(plain));
// Хендлер может вызвать api->send() → records_mu_ снова
// Без unlock была бы deadlock
```

---

## Разбивка по файлам

### `cm_identity.cpp`
- `bytes_to_hex()` — форматирование raw bytes → hex string
- `NodeIdentity::load_or_generate()` — главная точка входа
- `try_load_ssh_key()` — парсер OpenSSH Ed25519 (с `sodium_base642bin`)
- `load_or_gen_keypair()` — загрузка / генерация Ed25519 keypair + `chmod 0600`
- `save_key()` — запись ключа на диск

### `cm_session.cpp`
- `SessionState::encrypt()` — XSalsa20-Poly1305 + атомарный nonce
- `SessionState::decrypt()` — replay check + расшифровка + MAC verify
- `ConnectionManager::derive_session()` — X25519 ECDH + BLAKE2b-256 KDF

### `cm_handshake.cpp`
- `ConnectionManager()` конструктор, деструктор, `shutdown()`
- `fill_host_api()` — заполнение C-структуры коллбэков для плагинов
- `register_connector()` / `register_handler()`
- `handle_connect()` — новое соединение: запись в records_, генерация ephem_keypair, `send_auth()`
- `send_auth()` — формирование и отправка `MSG_TYPE_AUTH`
- `process_auth()` — верификация Ed25519, ECDH, `STATE_ESTABLISHED`, capability negotiation
- `handle_disconnect()` — очистка индексов, уведомление хендлеров через `handle_conn_state()`
- C-ABI адаптеры: `s_on_connect`, `s_on_data`, `s_on_disconnect`, `s_send`, `s_sign`, `s_verify`

### `cm_dispatch.cpp`
- `handle_data()` — TCP reassembly: накапливает `recv_buf`, вырезает полные пакеты
- `dispatch_packet()` — AUTH → `process_auth()`, остальное → decrypt → `bus_.emit()`

### `cm_send.cpp`
- `send()` — public API для хендлеров через `api->send()`
- `send_frame()` — encrypt → sign header → build frame → `connector->send_to()`
- `negotiate_scheme()` — выбор оптимального транспорта
- `resolve_uri()` — поиск `conn_id` по URI строке
- `local_schemes()` — список зарегистрированных коннекторов
- Геттеры: `connection_count()`, `get_active_uris()`, `get_state()`, `get_negotiated_scheme()`

---

## TCP Reassembly

```cpp
// cm_dispatch.cpp: handle_data()
{
    std::unique_lock lk(records_mu_);
    auto& rec = find_record(id); // throws if not found

    rec.recv_buf.insert(rec.recv_buf.end(), data, data + size);

    while (rec.recv_buf.size() >= sizeof(header_t)) {
        const auto* hdr =
            reinterpret_cast<const header_t*>(rec.recv_buf.data());

        if (hdr->magic != GNET_MAGIC) {
            LOG_WARN("conn #{}: bad magic, clearing recv_buf", id);
            rec.recv_buf.clear();
            break;
        }

        const size_t total = sizeof(header_t) + hdr->payload_len;
        if (rec.recv_buf.size() < total) break; // ждём остаток

        // Вырезаем полный пакет
        std::vector<uint8_t> pkt(
            rec.recv_buf.begin(),
            rec.recv_buf.begin() + total);
        rec.recv_buf.erase(
            rec.recv_buf.begin(),
            rec.recv_buf.begin() + total);

        lk.unlock();
        dispatch_packet(id, pkt);
        lk.lock();

        if (records_.find(id) == records_.end()) break; // соединение закрыто
    }
}
```

---

## host_api_t: как заполняется

```cpp
// cm_handshake.cpp: fill_host_api()
api->ctx              = this;
api->on_connect       = s_on_connect;    // static → handle_connect(ctx, ep)
api->on_data          = s_on_data;       // static → handle_data(ctx, id, raw, sz)
api->on_disconnect    = s_on_disconnect; // static → handle_disconnect(ctx, id, err)
api->send             = s_send;          // static → send(ctx, uri, type, data, sz)
api->sign_with_device = s_sign;          // static → Ed25519(device_seckey, data)
api->verify_signature = s_verify;        // static → Ed25519 verify
api->internal_logger  = get_logger_ptr();
api->plugin_type      = PLUGIN_TYPE_UNKNOWN; // PluginManager выставит перед init
```

Статические методы используются для совместимости с C function pointer (`void* ctx` как первый аргумент вместо `this`).

---

*← [04 — Криптография](04-crypto.md) · [06 — SignalBus →](06-signal-bus.md)*
