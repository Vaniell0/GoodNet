# Wire format

Каждый пакет на проводе — заголовок фиксированного размера + payload переменной длины.

См. также: [Noise_XX handshake](../protocol/noise-handshake.md) · [Криптография](../protocol/crypto.md) · [ConnectionManager](../architecture/connection-manager.md)

## Wire frame

```
┌────────── 20 bytes ──────────┬──── payload_len bytes ────┐
│         header_t             │        payload            │
└──────────────────────────────┴───────────────────────────┘
```

## header_t v3

```
Offset  Field         Bytes  Описание
──────  ────────────  ─────  ──────────────────────────────
  0     magic           4    0x474E4554 ('G','N','E','T')
  4     proto_ver       1    3
  5     flags           1    0x00 обычно, 0x01 = TRUSTED
  6     payload_type    2    MSG_TYPE_* (little-endian)
  8     payload_len     4    размер payload (little-endian)
 12     packet_id       8    монотонный per-connection counter
──────────────────────────────
                       20    total
```

`sizeof(header_t) == 20` — `static_assert` в `sdk/types.h`. Изменение layout сломает wire compatibility → `GNET_PROTO_VER` увеличится.

**Отличия от v2**: удалены `timestamp` (8 байт) и `sender_id` (16 байт). Timestamp не использовался на практике. Sender_id больше не нужен — [Noise_XX](../protocol/noise-handshake.md) сессия привязана к peer identity через DH-операции, идентификация отправителя до дешифрации не требуется.

**packet_id** — монотонный per-connection counter. Двойное назначение: [AEAD nonce](../protocol/crypto.md#nonce) (4 нулевых байта + 8 байт LE = 12-байтовый nonce) и дедупликация при [relay](../architecture/connection-manager.md#gossip-relay).

**flags**: `GNET_FLAG_TRUSTED` (0x01) — фрейм передаётся в открытом виде. Ядро принимает TRUSTED только от localhost-соединений (EP_FLAG_TRUSTED). Если удалённый узел пришлёт TRUSTED → drop.

### Binary layout example (hex dump)

Реальный зашифрованный фрейм (type=100, payload_len=128, packet_id=1):

```
Offset   Hex dump                                         ASCII / Описание
──────   ───────────────────────────────────────────────  ──────────────────
0x0000   47 4E 45 54                                      GNET    (magic)
0x0004   03                                               .       (proto_ver=3)
0x0005   00                                               .       (flags=0x00)
0x0006   64 00                                            d.      (type=100 LE)
0x0008   80 00 00 00                                      ....    (len=128 LE)
0x000C   01 00 00 00 00 00 00 00                          ........(pkt_id=1 LE)
────────────────────────────────────────────────────────────────────────────
         └─ header_t: 20 bytes total ─┘
────────────────────────────────────────────────────────────────────────────
0x0014   01                                               .       (flag=ZSTD)
0x0015   A0 00 00 00                                      ....    (orig_size=160)
0x0019   28 B5 2F FD ...                                  (/..    (zstd data)
  ...    ... зашифрованный payload (116 bytes) ...
0x008C   3F 12 A7 B4 C3 D9 E2 F1 08 AA BB CC DD EE FF 00  ?...    (MAC 16B)
────────────────────────────────────────────────────────────────────────────
Total: 20 (header) + 128 (payload) = 148 bytes
```

**Nonce derivation для дешифрации:**
```
packet_id = 0x0000000000000001 (LE from header offset 0x000C)
nonce[12] = {0x00, 0x00, 0x00, 0x00,  ← 4 zero bytes
             0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}  ← packet_id LE
```

**Дешифрация:**
```
ChaChaPoly_decrypt(key=recv_key, nonce=nonce, ciphertext=payload[0x0014:0x008C])
  → plaintext: flag(1) + orig_size(4) + zstd_data(116)
  → zstd_decompress(zstd_data, orig_size=160)
  → final_payload[160 bytes]
```

## Payload format

### До ESTABLISHED (Noise handshake)

Payload передаётся в открытом виде. Noise сообщения (NOISE_INIT, NOISE_RESP, NOISE_FIN) содержат Noise handshake data. Шифрование сессионными ключами ещё не установлено (но Noise_XX внутренне шифрует части msg2 и msg3 промежуточными ключами из DH chain).

### После ESTABLISHED

```
payload = ChaChaPoly-IETF(body) + MAC(16)

nonce(12) = 0x00[4] + packet_id_le(8)
  — nonce не передаётся на проводе, вычисляется из packet_id в header

body = flag(1) + [orig_size(4) if flag==ZSTD] + data
  flag=0x00 (RAW): data — исходный payload
  flag=0x01 (ZSTD): orig_size + zstd(payload, level=1)
```

Nonce вычисляется из `packet_id` заголовка: 4 нулевых байта + 8 байт `packet_id` (little-endian) = 12-байтовый nonce для ChaChaPoly-IETF. Nonce не передаётся на проводе — экономия 8 байт на каждом пакете. Монотонность `packet_id` гарантирует уникальность nonce.

Zstd включается автоматически для payload > 512 байт (настраивается в [compression config](../config.md#compressionconfig)), если сжатый размер меньше оригинала. Иначе отправляется RAW.

#### Compression decision flowchart

```
┌──────────────────────────────────────────┐
│ build_frame(payload)                     │
└────────────┬─────────────────────────────┘
             ▼
    payload.size() > cfg.compression.threshold (default 512)?
         │                   │
         NO                  YES
         │                   │
         │                   ▼
         │          ┌────────────────────────────┐
         │          │ compressed = zstd_compress │
         │          │ (payload, level=1)         │
         │          └────────┬───────────────────┘
         │                   │
         │                   ▼
         │          compressed.size() < payload.size()?
         │               │            │
         │               YES          NO
         │               │            │
         ▼               ▼            ▼
    ┌────────────────────────────────────────────┐
    │ body = flag + data                         │
    ├────────────────────────────────────────────┤
    │ flag=0x00 (RAW)                            │
    │ data = payload                             │
    │ Total: 1 + payload.size()                  │
    └────────┬───────────────────────────────────┘
             │               │
             │               ▼
             │          ┌──────────────────────────┐
             │          │ flag=0x01 (ZSTD)         │
             │          │ orig_size = payload.size │
             │          │ data = compressed        │
             │          │ Total: 1+4+compressed.size│
             │          └──────────┬───────────────┘
             │                     │
             └─────────┬───────────┘
                       ▼
              ┌────────────────────┐
              │ encrypt(body)      │
              │ ChaChaPoly-IETF    │
              └────────┬───────────┘
                       ▼
              [ ciphertext + MAC(16) ]
```

**Примеры:**
- payload=400 bytes → threshold не достигнут → RAW (1 + 400 = 401 byte body)
- payload=1000 bytes → compress → 600 bytes → 600 < 1000 → ZSTD (1 + 4 + 600 = 605 byte body)
- payload=1000 bytes → compress → 1050 bytes → 1050 > 1000 → RAW (1 + 1000 = 1001 byte body)

**Настройка compression:**
```cpp
cfg.compression.enabled = true;   // Включить/выключить
cfg.compression.threshold = 1024; // Минимальный размер (bytes)
cfg.compression.level = 3;        // Уровень zstd (1-22, выше=медленнее+лучше)
```

### Localhost (TRUSTED)

Payload = raw data, без шифрования, без compression flag. Ядро ставит `GNET_FLAG_TRUSTED` в header.

#### Localhost passthrough mode

Когда соединение помечено как localhost (`is_localhost == true`) и handshake завершён, ядро устанавливает `localhost_passthrough = true` (см. `finalize_handshake()` в `core/cm/handshake.cpp`). В этом режиме `build_frame()` обходит шифрование и сжатие:

```
if (rec->localhost_passthrough && !is_handshake):
  → header.flags = GNET_FLAG_TRUSTED
  → payload передаётся как есть (raw data)
  → без ChaChaPoly encrypt
  → без zstd compress
```

Определение localhost: `"127.0.0.1"`, `"::1"`, `"localhost"`, или `"127.*"` (проверка в `is_localhost_address()`).

Handshake-сообщения (NOISE_INIT/RESP/FIN) **не** проходят через passthrough — Noise handshake выполняется полностью даже для localhost, чтобы установить peer identity. Пропускается только post-ESTABLISHED трафик.

## Типы сообщений

### Core (0x00–0x0F)

Обрабатываются внутри [ConnectionManager](../architecture/connection-manager.md), не попадают в [SignalBus](../architecture/signal-bus.md).

| Константа | Значение | Payload |
|-----------|---------|---------|
| `MSG_TYPE_NOISE_INIT` | 1 | Noise msg1 (→e), только ephemeral key (32 bytes), без [HandshakePayload](../protocol/noise-handshake.md#handshakepayload) |
| `MSG_TYPE_NOISE_RESP` | 2 | Noise msg2 (←e,ee,s,es) + encrypted payload |
| `MSG_TYPE_NOISE_FIN` | 3 | Noise msg3 (→s,se) + encrypted payload |
| `MSG_TYPE_HEARTBEAT` | 4 | [HeartbeatPayload](../architecture/connection-manager.md#heartbeat) (16 bytes) |
| `MSG_TYPE_RELAY` | 10 | [RelayPayload](../architecture/connection-manager.md#gossip-relay)(33) + inner_frame |
| `MSG_TYPE_ICE_SIGNAL` | 11 | SDP blob (variable) |

### System services (0x0100–0x0FFF)

Зарезервированы для системных сервисов. Перехватываются SystemServiceDispatcher до пользовательских [handlers](../guides/handler-guide.md).

| Диапазон | Сервис | Типы |
|---------|--------|------|
| 0x0100–0x0102 | DHT | ping, find_node, announce |
| 0x0200–0x0202 | Health | ping, pong, report |
| 0x0300–0x0301 | RPC | request, response |
| 0x0400–0x0401 | Routing | route_announce, route_query |
| 0x0500–0x0501 | TUN/TAP | config, data |
| 0x0600–0x0606 | Store | put, get, result, delete, subscribe, notify, sync |

#### Store (0x0600–0x0606)

Распределённый key-value реестр. Ключи используют namespace-конвенцию: `"peer/<pubkey_hex>"`, `"service/<name>"`, `"route/<dest_hex>"`.

| Константа | Значение | Payload struct | Описание |
|-----------|---------|----------------|----------|
| `MSG_TYPE_SYS_STORE_PUT` | 0x0600 | `StorePutPayload` (24B) + key + value | Запись key-value (ttl, flags: replicate/overwrite_only) |
| `MSG_TYPE_SYS_STORE_GET` | 0x0601 | `StoreGetPayload` (16B) + key | Запрос по ключу (exact/prefix/list namespace) |
| `MSG_TYPE_SYS_STORE_RESULT` | 0x0602 | `StoreResultPayload` (16B) + entries | Ответ с результатами (entry_count * StoreEntry) |
| `MSG_TYPE_SYS_STORE_DELETE` | 0x0603 | `StoreDeletePayload` (16B) + key | Удаление записи |
| `MSG_TYPE_SYS_STORE_SUBSCRIBE` | 0x0604 | `StoreSubscribePayload` (16B) + key | Подписка на изменения ключа (exact/prefix) |
| `MSG_TYPE_SYS_STORE_NOTIFY` | 0x0605 | `StoreNotifyPayload` (12B) + StoreEntry | Уведомление об изменении (created/updated/deleted/expired) |
| `MSG_TYPE_SYS_STORE_SYNC` | 0x0606 | `StoreSyncPayload` (20B) + entries | Синхронизация между store-ами (full/delta) |

Ограничения: `STORE_KEY_MAX_LEN = 128`, `STORE_VALUE_MAX_LEN = 4096`. Payload struct-ы определены в `core/data/messages.hpp`.

### User (0x1000+)

Пользовательские типы. Обрабатываются [handler-плагинами](../guides/handler-guide.md) через [SignalBus](../architecture/signal-bus.md).

Предопределённые (для удобства): `MSG_TYPE_CHAT` (100), `MSG_TYPE_FILE` (200).

## TCP framing

TCP connector (`plugins/connectors/tcp/tcp.cpp`) использует двухфазное чтение:

```
Phase 1: async_read(socket, frame_buf, 20 bytes)
  → Прочитать header
  → Проверить magic + proto_ver
  → Если payload_len == 0 → deliver header-only frame
  → Если payload_len > 64 MB → protocol error, close

Phase 2: async_read(socket, frame_buf + 20, payload_len)
  → Прочитать payload
  → notify_data(id, frame_buf, 20 + payload_len)
  → goto Phase 1
```

`frame_buf` — contiguous buffer per connection. Capacity растёт до максимального размера фрейма и остаётся (zero-alloc после warmup).

## ICE signaling

ICE connector использует TCP как signaling channel. SDP exchange через `MSG_TYPE_ICE_SIGNAL` (type=11) — обычный payload в зашифрованном канале.

```
1. TCP handshake (Noise_XX) → ESTABLISHED
2. ICE connector отправляет SDP offer через TCP
3. Peer отвечает SDP answer через TCP
4. ICE negotiation (STUN/TURN)
5. UDP data channel established
```

---

**См. также:** [Noise_XX handshake](../protocol/noise-handshake.md) · [Криптография](../protocol/crypto.md) · [ConnectionManager](../architecture/connection-manager.md) · [Connector: гайд](../guides/connector-guide.md)
