# 08 — Протокол, криптография и соединения

`sdk/types.h` · `core/data/messages.hpp` · `core/cm_*.cpp` · `core/cm_identity.cpp`

Единый документ о wire-формате, рукопожатии, шифровании, идентификации и управлении соединениями.

---

## Wire-формат: header_t (v2, 44 байта)

```
Offset  Size  Поле           Описание
────────────────────────────────────────────────────────────────────
0       4     magic          0x474E4554 ('GNET')
4       1     proto_ver      2
5       1     flags          GNET_FLAG_TRUSTED (0x01) = plaintext
6       2     payload_type   MSG_TYPE_* (uint16_t)
8       4     payload_len    байты после заголовка
12      8     packet_id      монотонный счётчик соединения
20      8     timestamp      unix microseconds
28      16    sender_id      первые 16 байт device_pubkey отправителя
────────────────────────────────────────────────────────────────────
Итого: 44 байта  (#pragma pack(push,1), static_assert)
```

`sender_id[16]` — позволяет идентифицировать отправителя до расшифровки (~10 нс memcmp). При несовпадении — Early Drop без затрат на `crypto_secretbox_open` (~500 нс).

---

## FSM состояний соединения

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

Все состояния: `STATE_CONNECTING`, `STATE_AUTH_PENDING`, `STATE_KEY_EXCHANGE`, `STATE_ESTABLISHED`, `STATE_CLOSING`, `STATE_BLOCKED`, `STATE_CLOSED`.

---

## Рукопожатие

GoodNet использует симметричный протокол аутентификации — взаимная Auth без PKI, без CA. Доверие строится на out-of-band обмене `user_pubkey`.

### Обзор этапов

```
1. TCP connect
2. AUTH (оба узла одновременно) ── Ed25519 подпись + ephem_pk + CoreMeta
3. ECDH (локально)              ── X25519 + BLAKE2b-256 → session_key
4. ESTABLISHED                  ── весь трафик XSalsa20-Poly1305 + Zstd
```

### Диаграмма

```
Node A                          Wire                       Node B
  │                                                           │
  │◀── TCP connect ────────────────────────────────────────── │
  │                                                           │
  │ gen ephem_keypair()                     gen ephem_keypair()
  │                                                           │
  │── MSG_TYPE_AUTH ─────────────────────────────────────────▶│
  │   [user_pk|device_pk|sig|ephem_pk|schemes|CoreMeta]       │
  │                                                           │
  │◀─ MSG_TYPE_AUTH ──────────────────────────────────────────│
  │   [user_pk|device_pk|sig|ephem_pk|schemes|CoreMeta]       │
  │                                                           │
  │ verify Ed25519                          verify Ed25519    │
  │ session_key = BLAKE2b(X25519‖min‖max)  session_key = …    │
  │ STATE → ESTABLISHED                     STATE → ESTABLISHED
  │ sodium_memzero(ephem)                   sodium_memzero(…) │
  │                                                           │
  │── MSG_TYPE_CHAT ─────────────────────────────────────────▶│
  │   header ‖ nonce[8] ‖ XSalsa20-Poly1305(payload)          │
```

### AuthPayload (wire-формат)

```
Offset  Size  Поле           Описание
─────────────────────────────────────────────────────────────────
0       32    user_pubkey    Ed25519 публичный ключ пользователя
32      32    device_pubkey  Ed25519 публичный ключ устройства
64      64    signature      Ed25519(user_seckey, user_pk‖device_pk‖ephem_pk)
128     32    ephem_pubkey   X25519 эфемерный ключ для ECDH
─────────────────────────────────────────────────────────────────
kBaseSize = 160 байт   ← старые клиенты без capability negotiation
─────────────────────────────────────────────────────────────────
160     1     schemes_count  Количество схем (0–8)
161     128   schemes[8][16] Схемы: "tcp\0", "ws\0", …
─────────────────────────────────────────────────────────────────
289     8     core_meta      CoreMeta { core_version[4], caps_mask[4] }
─────────────────────────────────────────────────────────────────
kFullSize = 297 байт
```

Подпись покрывает `user_pk ‖ device_pk ‖ ephem_pk` (96 байт). Включение `ephem_pubkey` — защита от Replay Attack.

### CoreMeta — обмен возможностями

```c
struct CoreMeta {
    uint32_t core_version; // GN_CORE_VERSION
    uint32_t caps_mask;    // CORE_CAP_* флаги
};

#define CORE_CAP_ZSTD   (1U << 0) // Zstd сжатие
#define CORE_CAP_ICE    (1U << 1) // ICE/DTLS транспорт
#define CORE_CAP_KEYROT (1U << 2) // On-line ротация ключей
#define CORE_CAP_RELAY  (1U << 3) // Gossip relay
```

Старые клиенты присылают `payload_len == kBaseSize` → `peer_meta` = нулевой.

### Проверка AUTH

```cpp
// cm_auth.cpp: process_auth()
uint8_t to_verify[96];
memcpy(to_verify,      ap->user_pubkey,   32);
memcpy(to_verify + 32, ap->device_pubkey, 32);
memcpy(to_verify + 64, ap->ephem_pubkey,  32);

if (crypto_sign_ed25519_verify_detached(
        ap->signature, to_verify, 96, ap->user_pubkey) != 0) {
    // AUTH signature invalid → drop
    return false;
}

// Читаем schemes и CoreMeta, если размер позволяет
if (size >= kBaseSize + kSchemeBlock) peer_schemes = ap->get_schemes();
if (size >= kFullSize) peer_meta = ap->core_meta;
```

---

## ECDH и деривация сессионного ключа

```cpp
// cm_session.cpp: derive_session()

// 1. ECDH
uint8_t shared[32];
crypto_scalarmult(shared, sess.my_ephem_sk, peer_ephem_pk);

// 2. Domain separation: сортируем user_pk лексикографически
const uint8_t* pk_a = identity_.user_pubkey;
const uint8_t* pk_b = peer_user_pk;
if (std::memcmp(pk_a, pk_b, 32) > 0) std::swap(pk_a, pk_b);

// 3. KDF: BLAKE2b-256(shared ‖ pk_min ‖ pk_max)
crypto_generichash_state st;
crypto_generichash_init(&st, nullptr, 0, 32);
crypto_generichash_update(&st, shared, 32);
crypto_generichash_update(&st, pk_a,   32);
crypto_generichash_update(&st, pk_b,   32);
crypto_generichash_final(&st, sess.session_key, 32);

// 4. Затираем промежуточные данные
sodium_memzero(shared, sizeof(shared));
sess.clear_ephemeral();
sess.ready = true;
```

**Почему сортировать ключи?** Без сортировки обе стороны получат разные хэши (`BLAKE2b(shared‖A‖B)` ≠ `BLAKE2b(shared‖B‖A)`). Сортировка делает результат идентичным.

---

## Криптографические примитивы

| Операция | Примитив libsodium | Ключ/вывод |
|---|---|---|
| Подпись AUTH | `crypto_sign_ed25519` | sk: 64 B, pk: 32 B, sig: 64 B |
| ECDH | `crypto_scalarmult` (X25519) | shared: 32 B |
| KDF | `crypto_generichash` (BLAKE2b-256) | 32 B |
| Шифрование пакетов | `crypto_secretbox_easy` (**XSalsa20-Poly1305**) | MAC: 16 B |
| Сжатие (>512 B) | Zstd level 1 | — |
| Хэш hardware ID | `crypto_generichash` (BLAKE2b-256) | 32 B |
| SHA-256 манифестов | `crypto_hash_sha256` | 32 B |
| Безопасное затирание | `sodium_memzero` | — |

Все примитивы — libsodium. Никаких самописных алгоритмов.

---

## Шифрование и сжатие пакетов

### Отправка

**Малые пакеты (≤ 512 байт)** — без сжатия:

```
Wire = nonce_u64_le[8] ‖ XSalsa20-Poly1305(plain, nonce24, session_key)
overhead = 8 (nonce) + 16 (MAC) = 24 байта
```

**Большие пакеты (> 512 байт)** — Zstd + encrypt:

```
compressed = orig_size_u32_le[4] ‖ Zstd(plain, level=1)
Wire = nonce_u64_le[8] ‖ XSalsa20-Poly1305(compressed, nonce24, session_key)
```

Если Zstd завершился ошибкой — данные отправляются без сжатия с тем же `orig_size` префиксом.

### Приём

```
1. Извлечь nonce (первые 8 байт, little-endian)
2. Replay protection: nonce < expected → drop
3. Decrypt: crypto_secretbox_open_easy()
4. Если plaintext ≤ 512 байт → вернуть как есть
5. Иначе прочитать orig_size (первые 4 байта)
6. ZSTD_decompress() → оригинальные данные
```

### Replay protection

`send_nonce` — `fetch_add(1, relaxed)` при каждой отправке. `recv_nonce_expected` — монотонно возрастает; `nonce < expected` → `DropReason::ReplayDetected`.

---

## Localhost-оптимизация

Коннектор выставляет `EP_FLAG_TRUSTED` для loopback-адресов. CM читает этот флаг:

| Шаг | Поведение |
|---|---|
| AUTH | Выполняется полностью |
| ECDH | Выполняется |
| Шифрование payload | **Пропускается** |
| Zstd сжатие | **Пропускается** |
| Wire header flags | `GNET_FLAG_TRUSTED (0x01)` |

На приёме: фрейм с `GNET_FLAG_TRUSTED` от не-localhost → `DropReason::TrustedFromRemote` + drop.

---

## Идентификация

### NodeIdentity

```cpp
struct NodeIdentity {
    uint8_t user_pubkey  [32]; // Ed25519 — идентификация пользователя
    uint8_t user_seckey  [64]; // Ed25519 — подпись AUTH
    uint8_t device_pubkey[32]; // Ed25519 — идентификация устройства
    uint8_t device_seckey[64]; // Ed25519 — подпись заголовков
};
```

Создаётся через `NodeIdentity::load_or_generate(IdentityConfig)`.

### Ключевые пары

**user_key** — идентификатор пользователя, переносимый. Может быть SSH-ключом.

```
Приоритет загрузки:
  1. cfg.identity.ssh_key_path       → OpenSSH Ed25519 parser
  2. ~/.ssh/id_ed25519               → автодетект
  3. Генерация → <dir>/user_key      → 64 байта raw, chmod 0600
```

**device_key** — привязан к железу. Детерминированно восстанавливается:

```cpp
seed = BLAKE2b-256(machine_id ‖ user_pubkey)
device_key = crypto_sign_seed_keypair(seed)
sodium_memzero(seed)
```

Два ключа: `user_key` = "кто ты", `device_key` = "с какой машины". Компрометация `device_key` не раскрывает `user_key`.

### MachineId — источники по платформам

**Linux:** `/etc/machine-id`, `/var/lib/dbus/machine-id`, SMBIOS UUID, board serial, CPUID leaf 3, MAC физических сетевых карт.

**macOS:** IOKit Platform UUID, Serial Number, MAC через `getifaddrs()`.

**Windows:** MachineGuid (реестр), SMBIOS UUID, MAC через `GetAdaptersInfo()`.

Если ни один источник недоступен → `randombytes_buf()`, сохранение в `<dir>/machine_id`.

### Ротация ключей

```cpp
void ConnectionManager::rotate_identity_keys(const IdentityConfig& cfg) {
    NodeIdentity next = NodeIdentity::load_or_generate(cfg);
    {
        std::unique_lock lk(identity_mu_);
        identity_ = std::move(next);
    }
}
```

Без остановки сервера. Существующие сессии не прерываются — используют уже установленный `session_key`. Новые соединения получат новые ключи в AUTH.

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

    gn::msg::CoreMeta peer_core_meta{};
    std::string affinity_plugin;

    std::atomic<uint64_t> send_packet_id{0};

    std::unique_ptr<SessionState> session;
    std::vector<uint8_t>          recv_buf;
};
```

### Session affinity

После первого `PROPAGATION_CONSUMED` имя хендлера сохраняется в `affinity_plugin`. Сейчас — диагностика. В beta — hint для оптимизации порядка dispatch.

---

## Backpressure

```
Лимиты:
  • Global in-flight:     512 MB (MAX_IN_FLIGHT_BYTES)
  • Per-connection queue:  8 MB  (PerConnQueue)
  • Chunk size:            1 MB  (CHUNK_SIZE)
```

При превышении глобального лимита: `DropReason::Backpressure`, пакет отбрасывается. Буферы больше `CHUNK_SIZE * 2` нарезаются на чанки по 1 MB.

`get_pending_bytes()` — мониторинг текущей загрузки очереди.

---

## TCP Reassembly и Fast Path

```cpp
// cm_dispatch.cpp: handle_data()

// Fast path: полный фрейм, recv_buf пуст → zero-copy dispatch
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
```

TCP-коннектор отправляет ровно один полный фрейм за `notify_data()`. Когда `recv_buf` пуст — fast path диспетчеризирует напрямую из входящего указателя без копирования.

---

## Capability Negotiation

После получения `peer_schemes` из AUTH ядро выбирает оптимальный транспорт:

```cpp
// cm_send.cpp: negotiate_scheme()
for (const auto& prio : scheme_priority_) {
    if (!has_local(prio)) continue;
    if (rec.peer_schemes.empty()) return prio;
    if (has_peer(rec.peer_schemes, prio)) return prio;
}
return local_schemes().empty() ? "tcp" : local_schemes().front();
```

`scheme_priority_` по умолчанию: `["tcp", "ws", "udp", "mock", "ice"]`.

---

## Управление памятью секретов

| Данные | Когда затирается |
|---|---|
| `ephem_sk`, `ephem_pk` | Сразу после `derive_session()` |
| ECDH `shared` | Сразу после BLAKE2b final |
| `device_key seed` | Сразу после `crypto_sign_seed_keypair()` |
| `session_key` | В деструкторе `~SessionState()` |

`sodium_memzero` гарантирует, что компилятор не выбросит затирание как dead store.

---

## Thread safety

```
records_mu_     shared_lock (read) / unique_lock (mutate)
connectors_mu_  shared_lock / unique_lock
handlers_mu_    shared_lock / unique_lock
uri_mu_         shared_lock / unique_lock
pk_mu_          shared_lock / unique_lock
identity_mu_    shared_lock (send_auth) / unique_lock (rotate)
```

В `dispatch_packet`: `records_mu_` освобождается перед вызовом `bus_.dispatch_packet()` → хендлер может безопасно вызвать `send_response()`.

---

## Разбивка по файлам

| Файл | Что внутри |
|---|---|
| `cm_identity.cpp` | `NodeIdentity::load_or_generate`, OpenSSH Ed25519 parser |
| `cm_session.cpp` | `SessionState::encrypt/decrypt` (XSalsa20-Poly1305 + Zstd), `derive_session` |
| `cm_auth.cpp` | `send_auth`, `process_auth`, ICE init |
| `cm_handshake.cpp` | Конструктор CM, `fill_host_api`, `register_*`, `handle_connect/disconnect`, C-ABI trampolines |
| `cm_dispatch.cpp` | `handle_data` (TCP reassembly + fast path), `dispatch_packet` (Early Drop, decrypt, bus emit) |
| `cm_send.cpp` | `send`, `send_on_conn`, `build_frame`, `send_frame`, `flush_frames_to_connector`, `negotiate_scheme` |

---

*← [07 — Архитектура](07-architecture.md) · [09 — Безопасность →](09-security.md)*
