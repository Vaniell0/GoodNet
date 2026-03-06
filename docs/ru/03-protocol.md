# 03 — Протокол рукопожатия

GoodNet использует упрощённый вариант **Noise_XX** — взаимная аутентификация без центрального PKI, без сертификатов, без CA. Доверие строится на Out-of-Band обмене `user_pubkey`.

---

## Обзор этапов

```
1. TCP connect
2. AUTH (оба узла одновременно) ──── Ed25519 подпись + ephem_pk
3. ECDH (локально)              ──── X25519 + BLAKE2b-256 → session_key
4. ESTABLISHED                  ──── весь трафик XSalsa20-Poly1305
```

---

## Шаг 1 — AUTH

Сразу после TCP connect **оба** узла отправляют `MSG_TYPE_AUTH`. Порядок не важен — обмен симметричен. Не нужно ждать, кто инициировал соединение.

### Wire-формат `auth_payload_t`

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
161     128   schemes[8][16] Схемы: "tcp\0", "ws\0", …  (8 слотов × 16 байт)
─────────────────────────────────────────────────────────────────
kFullSize = 289 байт
```

### Что подписывается

```
to_sign   = user_pubkey[32] ‖ device_pubkey[32] ‖ ephem_pubkey[32]  (96 байт)
signature = Ed25519(user_seckey, to_sign)
```

Включение `ephem_pubkey` в подпись — ключевая защита от Replay Attack: перехваченный AUTH бесполезен, потому что `ephem_seckey` уже уничтожен, а переиспользовать `ephem_pubkey` из старого AUTH нельзя без `user_seckey`.

### Проверка на принимающей стороне

```cpp
// cm_handshake.cpp: process_auth()
uint8_t to_verify[96];
std::memcpy(to_verify,      ap->user_pubkey,   32);
std::memcpy(to_verify + 32, ap->device_pubkey, 32);
std::memcpy(to_verify + 64, ap->ephem_pubkey,  32);

if (crypto_sign_ed25519_verify_detached(
        ap->signature, to_verify, 96, ap->user_pubkey) != 0) {
    LOG_WARN("conn #{}: AUTH signature invalid", id);
    return false;
}
rec.peer_user_pubkey   = ap->user_pubkey;
rec.peer_device_pubkey = ap->device_pubkey;
rec.peer_authenticated = true;
```

### Backward compatibility

Старые клиенты отправляют `payload_len == 160` (kBaseSize). Ядро проверяет размер:

```cpp
if (plen >= kFullSize) {
    rec.peer_schemes = parse_schemes(ap->schemes, ap->schemes_count);
}
// иначе: peer_schemes остаётся пустым → negotiate_scheme() выбирает первую локальную
```

---

## Шаг 2 — ECDH (локально, без сети)

После успешной верификации AUTH оба узла независимо вычисляют одинаковый `session_key`:

```
shared      = X25519(my_ephem_sk, peer_ephem_pk)    // 32 байта
pk_min      = min(my_user_pk, peer_user_pk)          // лексикографически
pk_max      = max(my_user_pk, peer_user_pk)
session_key = BLAKE2b-256(shared ‖ pk_min ‖ pk_max) // 32 байта
```

**Почему сортировать ключи?** Без сортировки узел A вычислил бы `BLAKE2b(shared ‖ A_pk ‖ B_pk)`, а B — `BLAKE2b(shared ‖ B_pk ‖ A_pk)`. Разные хэши — сессия не установится. Лексикографическая сортировка делает результат детерминированным независимо от направления.

**Почему включать user_pk в KDF?** Domain separation: два разных пользователя со случайно совпавшими эфемерными ключами (теоретически возможно) получат разные `session_key`, потому что их `user_pk` различаются.

После деривации:
```cpp
sodium_memzero(sess.my_ephem_sk, sizeof(sess.my_ephem_sk));
sodium_memzero(sess.my_ephem_pk, sizeof(sess.my_ephem_pk));
sess.ready = true;
rec.state  = STATE_ESTABLISHED;
```

---

## Шаг 3 — ESTABLISHED

### Шифрование пакетов

```
Wire payload = nonce_u64_le[8] ‖ XSalsa20-Poly1305(plain, nonce24, session_key)

nonce24 = nonce_u64 (LE, 8 байт) ‖ 0x00[16]
overhead = 8 (nonce prefix) + 16 (Poly1305 MAC) = 24 байта/пакет
```

`send_nonce` атомарно инкрементируется при каждой отправке (`fetch_add`, `memory_order_relaxed`).
`recv_nonce_expected` монотонно возрастает — `nonce < expected` → drop (Replay Attack).

### Подпись заголовков

```cpp
// cm_send.cpp: send_frame(), только для не-localhost ESTABLISHED
const size_t body = offsetof(header_t, signature);
crypto_sign_ed25519_detached(
    hdr.signature, nullptr,
    reinterpret_cast<const uint8_t*>(&hdr), body,
    identity_.device_seckey);
```

---

## Capability Negotiation

После получения `peer_schemes` из AUTH ядро выбирает оптимальный транспорт:

```cpp
// cm_send.cpp: negotiate_scheme()
// Итерируем scheme_priority_ (настраивается, дефолт: ["tcp","ws","udp","mock"])
for (const auto& prio : scheme_priority_) {
    if (!has_local(prio)) continue;           // у нас нет такого коннектора
    if (rec.peer_schemes.empty()) return prio; // старый клиент → берём первую нашу
    if (has_peer(rec.peer_schemes, prio)) return prio;
}
return local_schemes().empty() ? "tcp" : local_schemes().front();
```

**Пример:**
```
Локально:  [tcp, ws]
Пир:       [udp, ws, tcp]
Priority:  [tcp, ws, udp, mock]
Результат: "tcp"  ← первый из priority, поддерживаемый обеими сторонами
```

---

## Localhost-оптимизация

Адреса `127.x.x.x`, `::1`, `localhost` → `is_localhost = true`. Для таких соединений:

| Шаг | Поведение |
|---|---|
| AUTH | Выполняется полностью |
| ECDH | Выполняется (session создаётся) |
| Шифрование payload | **Пропускается** |
| Подпись заголовка | **Пропускается** |

Цель — нулевые накладные расходы при использовании GoodNet как IPC-шины между процессами на одной машине.

---

## Wire-формат `header_t`

```
Offset  Size  Поле           Описание
────────────────────────────────────────────────────────────────────
0       4     magic          0x474E4554 ('GNET') — защита от мусора
4       1     proto_ver      Версия протокола (текущая: 1)
5       1     flags          Зарезервировано, всегда 0
6       2     reserved       Зарезервировано, всегда 0
8       8     packet_id      Монотонный счётчик пакетов соединения
16      8     timestamp      Время отправки, unix microseconds
24      4     payload_type   MSG_TYPE_* константа
28      2     status         STATUS_OK(0) / STATUS_ERROR(1)
30      4     payload_len    Размер payload после заголовка (байт)
34      64    signature      Ed25519(device_sk, header[0..33])
                             Нулевые байты до STATE_ESTABLISHED
────────────────────────────────────────────────────────────────────
Итого: 98 байт  (#pragma pack(push,1), без выравнивания)
```

### Типы сообщений

| Константа | Значение | Описание |
|---|---|---|
| `MSG_TYPE_SYSTEM` | 0 | Системные, зарезервировано |
| `MSG_TYPE_AUTH` | 1 | Рукопожатие (всегда plaintext) |
| `MSG_TYPE_KEY_EXCHANGE` | 2 | Зарезервировано |
| `MSG_TYPE_HEARTBEAT` | 3 | Keepalive ping/pong |
| `MSG_TYPE_CHAT` | 100 | Текстовые сообщения |
| `MSG_TYPE_FILE` | 200 | Файловая передача |
| 1000–9999 | — | Пользовательские типы плагинов |
| 10000+ | — | Экспериментальные |

---

## Полная диаграмма рукопожатия

```
Node A                          Wire                       Node B
  │                                                           │
  │◀── TCP accept ─────────────────────────────────────────── │
  │                                                           │
  │ gen ephem_keypair()                     gen ephem_keypair()
  │                                                           │
  │── MSG_TYPE_AUTH ─────────────────────────────────────────▶│
  │   [user_pk|device_pk|sig|ephem_pk|schemes]                │
  │                                                           │
  │◀─ MSG_TYPE_AUTH ──────────────────────────────────────────│
  │   [user_pk|device_pk|sig|ephem_pk|schemes]                │
  │                                                           │
  │ verify Ed25519(peer_user_pk, sig)   verify Ed25519(...)   │
  │ shared = X25519(my_sk, peer_ephem)  shared = X25519(...)  │
  │ session_key = BLAKE2b(shared‖min‖max)  session_key = ...  │
  │ STATE → ESTABLISHED                 STATE → ESTABLISHED   │
  │ sodium_memzero(ephem_sk, ephem_pk)  sodium_memzero(...)   │
  │                                                           │
  │── MSG_TYPE_CHAT ─────────────────────────────────────────▶│
  │   header[signed] ‖ nonce[8] ‖ secretbox(payload)         │
```

---

*← [02 — Архитектура](02-architecture.md) · [04 — Криптография →](04-crypto.md)*
