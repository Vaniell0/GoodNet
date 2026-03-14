# 03 — Протокол рукопожатия

GoodNet использует упрощённый вариант **Noise_XX** — взаимная аутентификация без центрального PKI, без сертификатов, без CA. Доверие строится на Out-of-Band обмене `user_pubkey`.

---

## Обзор этапов

```
1. TCP connect
2. AUTH (оба узла одновременно) ──── Ed25519 подпись + ephem_pk + CoreMeta
3. ECDH (локально)              ──── X25519 + BLAKE2b-256 → session_key
4. ESTABLISHED                  ──── весь трафик XSalsa20-Poly1305 + Zstd
```

---

## Шаг 1 — AUTH

Сразу после TCP connect **оба** узла отправляют `MSG_TYPE_AUTH`. Порядок не важен — обмен симметричен.

### Wire-формат `gn::msg::AuthPayload`

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

### CoreMeta — обмен возможностями

```c
// data/messages.hpp
#define CORE_CAP_ZSTD   (1U << 0) // Zstd сжатие поддерживается
#define CORE_CAP_ICE    (1U << 1) // ICE/DTLS транспорт
#define CORE_CAP_KEYROT (1U << 2) // On-line ротация ключей

#define GN_CORE_VERSION ((1U << 16) | (0U << 8) | 2U)  // 1.0.2

struct CoreMeta {
    uint32_t core_version; // GN_CORE_VERSION отправителя
    uint32_t caps_mask;    // CORE_CAP_* флаги
};
```

Локальный узел объявляет: `caps_mask = CORE_CAP_ZSTD | CORE_CAP_KEYROT`.

Старые клиенты присылают `payload_len == kBaseSize` → `peer_meta` остаётся нулевым — получатель трактует всё как "unknown".

### Что подписывается

```
to_sign   = user_pubkey[32] ‖ device_pubkey[32] ‖ ephem_pubkey[32]  (96 байт)
signature = Ed25519(user_seckey, to_sign)
```

Включение `ephem_pubkey` в подпись — ключевая защита от Replay Attack.

### Проверка на принимающей стороне

```cpp
// cm_handshake.cpp: process_auth()
uint8_t to_verify[96];
memcpy(to_verify,      ap->user_pubkey,   32);
memcpy(to_verify + 32, ap->device_pubkey, 32);
memcpy(to_verify + 64, ap->ephem_pubkey,  32);

if (crypto_sign_ed25519_verify_detached(
        ap->signature, to_verify, 96, ap->user_pubkey) != 0) {
    LOG_WARN("conn #{}: AUTH signature invalid", id);
    return false;
}

// Читаем schemes и CoreMeta, если размер позволяет
if (size >= kBaseSize + kSchemeBlock)
    peer_schemes = ap->get_schemes();
if (size >= kFullSize)
    peer_meta = ap->core_meta;
```

---

## Шаг 2 — ECDH (локально, без сети)

```
shared      = X25519(my_ephem_sk, peer_ephem_pk)    // 32 байта
pk_min      = min(my_user_pk, peer_user_pk)          // лексикографически
pk_max      = max(my_user_pk, peer_user_pk)
session_key = BLAKE2b-256(shared ‖ pk_min ‖ pk_max) // 32 байта
```

После деривации:
```cpp
sodium_memzero(sess.my_ephem_sk, sizeof(sess.my_ephem_sk));
sodium_memzero(sess.my_ephem_pk, sizeof(sess.my_ephem_pk));
sess.ready = true;
rec.state  = STATE_ESTABLISHED;
```

---

## Шаг 3 — ESTABLISHED

### Шифрование и сжатие пакетов

**Малые пакеты (≤ 512 байт)** — без сжатия:
```
Wire = nonce_u64_le[8] ‖ XSalsa20-Poly1305(plain, nonce24, session_key)
overhead = 8 (nonce) + 16 (MAC) = 24 байта
```

**Большие пакеты (> 512 байт)** — со сжатием:
```
compressed = orig_size_u32_le[4] ‖ Zstd(plain, level=3)
Wire = nonce_u64_le[8] ‖ XSalsa20-Poly1305(compressed, nonce24, session_key)
```

Если Zstd завершился ошибкой — данные отправляются без сжатия с тем же префиксом `orig_size`.

При расшифровке: если размер plaintext ≤ 512 байт или `orig_size == 0` → возвращается как есть. Иначе → Zstd decompress по `orig_size`.

### Replay protection

`send_nonce` атомарно инкрементируется (`fetch_add`, `relaxed`).
`recv_nonce_expected` монотонно возрастает — `nonce < expected` → drop.

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

## Localhost-оптимизация

Адреса `127.x.x.x`, `::1`, `localhost` → `is_localhost = true`:

| Шаг | Поведение |
|---|---|
| AUTH | Выполняется полностью |
| ECDH | Выполняется |
| Шифрование payload | **Пропускается** |
| Zstd сжатие | **Пропускается** |
| Подпись заголовка | **Пропускается** |

---

## Wire-формат `header_t`

```
Offset  Size  Поле           Описание
────────────────────────────────────────────────────────────────────
0       4     magic          0x474E4554 ('GNET')
4       1     proto_ver      1
5       1     flags          зарезервировано, 0
6       2     reserved       зарезервировано, 0
8       8     packet_id      монотонный счётчик соединения
16      8     timestamp      unix microseconds
24      4     payload_type   MSG_TYPE_*
28      2     status         STATUS_OK(0) / STATUS_ERROR(1)
30      4     payload_len    байты после заголовка
34      64    signature      Ed25519(device_sk, header[0..33]); 0 до AUTH
────────────────────────────────────────────────────────────────────
Итого: 98 байт  (#pragma pack(push,1))
```

### Типы сообщений

| Константа | Значение | Описание |
|---|---|---|
| `MSG_TYPE_SYSTEM` | 0 | Системные |
| `MSG_TYPE_AUTH` | 1 | Рукопожатие (plaintext) |
| `MSG_TYPE_KEY_EXCHANGE` | 2 | Зарезервировано |
| `MSG_TYPE_HEARTBEAT` | 3 | Keepalive ping/pong |
| `MSG_TYPE_ICE_SIGNAL` | 11 | ICE/DTLS SDP обмен |
| `MSG_TYPE_CHAT` | 100 | Текстовые сообщения |
| `MSG_TYPE_FILE` | 200 | Файловая передача |
| 1000–9999 | — | Пользовательские типы |
| 10000+ | — | Экспериментальные |

---

## Полная диаграмма рукопожатия

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
  │   header[signed] ‖ nonce[8] ‖ XSalsa20-Poly1305(payload)  │
```

---

*← [02 — Архитектура](02-architecture.md) · [04 — Криптография →](04-crypto.md)*