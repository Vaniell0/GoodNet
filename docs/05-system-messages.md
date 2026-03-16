# 05 — Системные сообщения

`sdk/types.h` · `core/data/messages.hpp` · `core/cm_dispatch.cpp` · `core/cm_handshake.cpp`

Ядро GoodNet использует несколько типов сообщений для внутренних целей: аутентификация, обмен ключами, relay, ICE. Эти сообщения обрабатываются **автоматически** в `dispatch_packet()` — пользовательский код их не видит (за исключением relay и ICE, которые при необходимости переадресуются).

---

## Реестр типов сообщений

```c
// sdk/types.h — диапазоны
// 0–99     core (зарезервированы ядром)
// 100–999  built-in (стандартные типы)
// 1000+    user (пользовательские)

#define MSG_TYPE_SYSTEM        0u    // зарезервирован
#define MSG_TYPE_AUTH          1u    // аутентификация + ECDH
#define MSG_TYPE_KEY_EXCHANGE  2u    // ротация ключей
#define MSG_TYPE_HEARTBEAT     3u    // keepalive (запланирован)
#define MSG_TYPE_RELAY        10u    // gossip relay
#define MSG_TYPE_ICE_SIGNAL   11u    // ICE SDP exchange
#define MSG_TYPE_CHAT        100u    // чат (built-in пример)
#define MSG_TYPE_FILE        200u    // файл (built-in пример)
```

---

## MSG_TYPE_AUTH (1)

### Назначение

Взаимная аутентификация и установление зашифрованного канала. Отправляется обеими сторонами сразу после TCP-соединения, **в plaintext**.

### Формат: AuthPayload (297 байт)

```
Offset  Size  Поле
─────────────────────────────────────────────
0       32    user_pubkey       Ed25519 публичный ключ пользователя
32      32    device_pubkey     Ed25519 публичный ключ устройства
64      64    signature         Ed25519(user_sk, user_pk||device_pk||ephem_pk)
128     32    ephem_pubkey      X25519 одноразовый ключ для ECDH
────── kBaseSize = 160 ──────────────────────
160     1     schemes_count     количество поддерживаемых транспортов
161     128   schemes[8][16]    NUL-terminated: "tcp", "ice", ...
────── kSchemeBlock = 129 ───────────────────
289     8     CoreMeta          core_version(4) + caps_mask(4)
────── kFullSize = 297 ─────────────────────
```

Старые клиенты отправляют только `kBaseSize` (160 байт). Ядро проверяет `payload_len` перед чтением опциональных секций.

### Автоматика ядра

```
TCP connect
  │
  ├─ Обе стороны отправляют AuthPayload (plaintext)
  │
  ├─ Получение AUTH:
  │   1. crypto_sign_verify(signature, user_pk||device_pk||ephem_pk, user_pk)
  │   2. Если payload_len >= kFullSize:
  │      - прочитать schemes → сохранить в peer_schemes
  │      - прочитать CoreMeta → сохранить в peer_core_meta
  │   3. crypto_sign_ed25519_pk_to_curve25519(ephem_pk) → X25519
  │   4. crypto_scalarmult(shared, my_ephem_sk, peer_ephem_pk_x25519)
  │   5. session_key = BLAKE2b-256(shared || min(pk1,pk2) || max(pk1,pk2))
  │   6. sodium_memzero(ephem_sk, shared)
  │   7. STATE → ESTABLISHED
  │
  └─ Пользовательский код НЕ участвует
```

### CoreMeta — capability flags

```c
#define CORE_CAP_ZSTD   (1U << 0)  // zstd сжатие
#define CORE_CAP_ICE    (1U << 1)  // ICE/DTLS транспорт
#define CORE_CAP_KEYROT (1U << 2)  // ротация ключей
#define CORE_CAP_RELAY  (1U << 3)  // gossip relay

struct CoreMeta {
    uint32_t core_version;  // (major<<16)|(minor<<8)|patch
    uint32_t caps_mask;     // CORE_CAP_*
};
```

Старые клиенты: `peer_core_meta = {0, 0}` — все флаги выключены.

---

## MSG_TYPE_KEY_EXCHANGE (2)

### Назначение

Ротация session_key без разрыва соединения (Perfect Forward Secrecy).

### Формат: KeyExchangePayload (96 байт)

```
Offset  Size  Поле
─────────────────────────────────
0       32    x25519_pubkey    новый ephemeral X25519 ключ
32      64    signature        Ed25519(device_sk, x25519_pubkey)
```

### Автоматика ядра

```
core.rekey_session(conn_id)
  │
  ├─ Генерация нового ephemeral keypair
  ├─ Подпись device_sk → KeyExchangePayload
  ├─ Отправка пиру
  ├─ Пир верифицирует подпись, выполняет ECDH
  ├─ Новый session_key, sodium_memzero(старый ключ)
  └─ Существующие пакеты в очереди доставляются старым ключом
```

**Текущий статус**: `rekey_session()` реализован и работает. Standalone KEY_EXCHANGE (инициация пиром) — в процессе.

---

## MSG_TYPE_HEARTBEAT (3)

### Назначение

Keepalive ping/pong для обнаружения разрывов и измерения RTT.

### Формат: HeartbeatPayload (16 байт)

```
Offset  Size  Поле
─────────────────────────────────
0       8     timestamp_us     unix microseconds
8       4     seq              монотонный счётчик
12      1     flags            0x00=ping, 0x01=pong
13      3     _pad
```

### Текущий статус

**Определён, но НЕ отправляется ядром.** Структура зафиксирована в ABI, автоматическая отправка heartbeat запланирована на beta. Можно использовать вручную из хендлер-плагина:

```cpp
gn::msg::HeartbeatMessage ping;
ping->timestamp_us = /* now */;
ping->seq          = ++counter_;
ping->flags        = 0x00;
auto wire = ping.serialize();
send_response(peer, MSG_TYPE_HEARTBEAT, wire.data(), wire.size());
```

---

## MSG_TYPE_RELAY (10)

### Назначение

Gossip relay — пересылка пакетов через промежуточные узлы к конечному получателю по `dest_pubkey`.

### Формат: RelayPayload (33 байт + inner_frame)

```
Offset  Size  Поле
─────────────────────────────────
0       1     ttl              оставшиеся хопы (декремент, drop при 0)
1       32    dest_pubkey      Ed25519 user_pubkey получателя
33      ...   inner_frame      header_t + encrypted payload (opaque)
```

### Автоматика ядра

```
dispatch_packet(MSG_TYPE_RELAY)
  │
  ├─ ttl == 0 → drop (DropReason::RelayDropped)
  ├─ ttl-- (декремент)
  ├─ dest_pubkey == my user_pubkey?
  │   ├─ ДА → извлечь inner_frame → dispatch как обычный пакет
  │   └─ НЕТ → find_conn_by_pubkey(dest_pubkey)
  │            ├─ найден → forward inner_frame пиру
  │            └─ не найден → broadcast (gossip)
  └─ Дедупликация по packet_id
```

---

## MSG_TYPE_ICE_SIGNAL (11)

### Назначение

Обмен SDP (Session Description Protocol) между пирами для установления ICE/DTLS соединения. Передаётся через существующий TCP канал.

### Формат: IceSignalPayload (8 байт + SDP)

```
Offset  Size  Поле
─────────────────────────────────
0       1     kind         0=OFFER, 1=ANSWER
1       3     _pad
4       4     sdp_len      длина SDP-строки
8       ...   SDP          UTF-8 текст
```

### Автоматика ядра

ICE_SIGNAL маршрутизируется через обычный pipeline. ICE коннектор-плагин подписывается на этот тип:

```
Клиент: --ice-upgrade → connect("ice://" + peer_hex)
  │
  ├─ ICE connector генерирует OFFER (local SDP + candidates)
  ├─ Отправляет IceSignalPayload(kind=OFFER) через TCP
  │
  ├─ Пир получает OFFER → ICE connector генерирует ANSWER
  ├─ Отправляет IceSignalPayload(kind=ANSWER) через TCP
  │
  └─ Оба: ICE connectivity check → DTLS → прямой UDP канал
```

---

## Сводка автоматики

| Тип | Ядро делает автоматически | Требует код пользователя |
|---|---|---|
| **AUTH** | Подпись, верификация, ECDH, session_key, state transition | Нет |
| **KEY_EXCHANGE** | Верификация, derive, обновление session_key | `core.rekey_session(id)` для инициации |
| **HEARTBEAT** | Ничего (в alpha) | Ручная отправка из хендлера |
| **RELAY** | TTL декремент, дедупликация, forward/local delivery | Нет (если relay включён) |
| **ICE_SIGNAL** | Маршрутизация через pipeline | `--ice-upgrade` CLI или `core.connect("ice://...")` |

---

## Системные сервисы (0x0100-0x0FFF)

Новый слой **SystemServiceDispatcher** перехватывает сообщения в диапазоне `0x0100-0x0FFF` **до** пользовательских хендлеров. Регистрация через `ConnectionManager::system_services().register_service(type, handler)`.

### DHT / Service Discovery

| Тип | Код | Payload | Назначение |
|-----|-----|---------|------------|
| DHT_PING | 0x0100 | DhtPingPayload (44 B) | Ping/pong для проверки живости |
| DHT_FIND_NODE | 0x0101 | DhtFindNodePayload + N×DhtNodeEntry | Поиск узла по pubkey |
| DHT_ANNOUNCE | 0x0102 | DhtAnnouncePayload | Объявление присутствия |

### Health / Metrics

| Тип | Код | Payload | Назначение |
|-----|-----|---------|------------|
| HEALTH_PING | 0x0200 | HealthPingPayload (16 B) | Keepalive с метриками |
| HEALTH_PONG | 0x0201 | HealthPingPayload (16 B) | Ответ на ping |
| HEALTH_REPORT | 0x0202 | HealthReportPayload (32 B) | Полный отчёт о состоянии |

### Distributed RPC

| Тип | Код | Payload | Назначение |
|-----|-----|---------|------------|
| RPC_REQUEST | 0x0300 | RpcRequestPayload (16 B) + data | Вызов метода (FNV-1a hash) |
| RPC_RESPONSE | 0x0301 | RpcResponsePayload (16 B) + data | Результат вызова |

**RPC method hash**: `GN_RPC_HASH("method_name")` — compile-time FNV-1a для zero-overhead dispatch.

### Routing

| Тип | Код | Payload | Назначение |
|-----|-----|---------|------------|
| ROUTE_ANNOUNCE | 0x0400 | RouteAnnouncePayload (72 B) | Объявление маршрута |
| ROUTE_QUERY | 0x0401 | RouteQueryPayload + via_pubkey | Запрос маршрута |

### TUN/TAP

| Тип | Код | Payload | Назначение |
|-----|-----|---------|------------|
| TUN_CONFIG | 0x0500 | TunConfigPayload (48 B) | Конфигурация туннеля |
| TUN_DATA | 0x0501 | TunDataPayload (8 B) + IP packet | Инкапсулированный IP |

---

## Правила расширения

1. Core типы (0x00-0x0F): `#define MSG_TYPE_*` в `sdk/types.h`
2. System services (0x0100-0x0FFF): `#define MSG_TYPE_SYS_*` в `sdk/types.h`, register через `SystemServiceDispatcher`
3. User типы (0x1000+): определяются плагинами
4. Формат: `#pragma pack(push, 1)` struct в `core/data/messages.hpp`
5. `static_assert` на размер структуры
6. `kBaseSize` / `kFullSize` для forward compatibility

---

*← [04 — Плагины](04-plugins.md) · [06 — Методы применения →](06-usage-patterns.md)*
