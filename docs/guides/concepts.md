# Core Concepts

Ключевые концепции GoodNet: идентификация, routing, состояние соединений.

См. также: [Обзор архитектуры](../architecture.md) · [Handler гайд](../guides/handler-guide.md) · [Protocol: crypto](../protocol/crypto.md)

## Connection identity

### conn_id_t vs peer_id vs pubkey

**Три уровня идентификации:**

```cpp
conn_id_t     // Временный handle (TCP session lifetime)
peer_id       // Стабильный ID (пока connection жив)
pubkey[32]    // Постоянный identity (Ed25519 user key)
```

**conn_id_t** (`uint64_t`):
- Монотонный счётчик, выдаётся при `notify_connect()`
- Живёт только пока TCP/ICE session активен
- После disconnect — **никогда не переиспользуется**
- Используется для low-level операций: `send_response()`, `disconnect()`

**peer_id** (в `endpoint_t`):
- conn_id, назначенный ядром при `on_connect()` — используется для `send_response()`
- Используется для routing внутри Core

**pubkey** (`uint8_t[32]`):
- Ed25519 public key (постоянный identity)
- Один user key = один "аккаунт" в сети
- Переносится между устройствами
- Валиден после ESTABLISHED (заполняется ядром после Noise_XX handshake)
- Используется для адресации: `send("0xAAA...", type, data)`

**Пример:**
```
User Alice имеет:
  pubkey = 0xAAA...

TCP session 1 к Alice:
  conn_id = 42
  endpoint.peer_id = 42  (== conn_id)
  endpoint.pubkey = 0xAAA

После disconnect, новый connect:
  conn_id = 43  (новый!)
  endpoint.pubkey = 0xAAA  (тот же)
```

## endpoint_t structure

```c
typedef struct {
    char     address[128];                       // NUL-terminated IP or hostname
    uint16_t port;                               // Peer port (host order)
    uint8_t  pubkey[GN_SIGN_PUBLICKEYBYTES];     // Ed25519 user pubkey (valid after ESTABLISHED)
    uint64_t peer_id;                            // conn_id assigned by core on on_connect()
    uint8_t  flags;                              // EP_FLAG_* bitmask
} endpoint_t;
```

**Flags:**
- `EP_FLAG_TRUSTED` (0x01) — localhost connection (skip AEAD)
- `EP_FLAG_OUTBOUND` (0x02) — мы инициировали (Noise Initiator role)

**Lifetime:** `endpoint_t` валиден только внутри `handle_message()`. Если нужно сохранить — копируйте поля:

```cpp
// ❌ WRONG: сохранение указателя
const endpoint_t* saved_ep_;  // use-after-free!

// ✅ CORRECT: копирование данных
struct PeerInfo {
    conn_id_t peer_id;
    std::array<uint8_t, 32> pubkey;
};
std::unordered_map<conn_id_t, PeerInfo> active_peers_;
```

## Routing: URI formats

**Три способа адресации:**

### 1. TCP direct
```cpp
send("tcp://192.168.1.100:25565", type, data);
```
- Прямое TCP соединение
- Если не подключены → auto-connect
- Если уже есть connection → reuse

### 2. Pubkey routing
```cpp
send("0xAABBCCDD...", type, data);  // hex user_pubkey
```
- Ищет существующий connection с таким `user_pubkey`
- Если нет прямого → использует [relay](../architecture/connection-manager.md#gossip-relay)
- Если нет relay path → возвращает false

### 3. Peer ID routing
```cpp
// Внутри handler:
send_response(endpoint->peer_id, type, data);
```
- Быстрый ответ на conn_id
- Не требует URI resolution
- Гарантированно работает (connection уже есть)

## Connection lifecycle

```
[notify_connect] → CONNECTING
  ↓
[Noise handshake] → HANDSHAKE
  ├─ INIT_SENT → recv RESP → FIN_SENT
  └─ WAIT_INIT → recv INIT → RESP_SENT → recv FIN
  ↓
[Split() → keys] → ESTABLISHED
  ↓
[Heartbeat OK] → ACTIVE (30s loop)
  ↓
[3 missed PONG / close()] → CLOSING
  ↓
[drain queue] → CLOSED
  ↓
[notify_disconnect] → RCU remove
```

**Важно:** Сообщения можно отправлять **сразу** после `send()`, даже если соединение в HANDSHAKE. Core автоматически queues до ESTABLISHED.

## Message types: ranges

```
0x0000 - 0x000F   Core (NOISE_*, HEARTBEAT, RELAY)
                  Не доставляются в handlers

0x0010 - 0x00FF   Reserved (будущее использование)

0x0100 - 0x0FFF   System services (DHT, RPC, Health)
                  Intercepted до user handlers

0x1000 - 0xFFFF   User space (плагины)
                  Свободно для application
```

**Predefined user types:**
- `MSG_TYPE_CHAT = 100`
- `MSG_TYPE_FILE = 200`

**Custom types:**
```cpp
#define MY_GAME_MOVE  0x1000
#define MY_GAME_STATE 0x1001
```

## Propagation semantics

```cpp
enum propagation_t {
    PROPAGATION_CONTINUE = 0,  // Продолжить chain
    PROPAGATION_CONSUMED = 1,  // Pin affinity, stop chain
    PROPAGATION_REJECT   = 2,  // Drop packet, stop chain
};
```

**CONTINUE:**
- Handler обработал, но другие тоже могут
- Примеры: logger, metrics

**CONSUMED:**
- Handler взял ownership
- Последующие пакеты на этом conn → прямо к этому handler (skip chain)
- **~30x быстрее** (session affinity)
- Примеры: chat session, file transfer

**REJECT:**
- Пакет невалиден/неизвестен
- Останавливает chain, не создаёт affinity
- Примеры: bad payload format

## Zero-copy data access

**sdk::PodData<T>:**
```cpp
struct GameMove {
    int32_t player_id;
    float x, y, z;
};

void handle_message(..., std::span<const uint8_t> payload) {
    auto move = sdk::PodData<GameMove>::from_span(payload);
    if (!move) return PROPAGATION_REJECT;  // invalid size

    // Zero-copy: move.get() → const GameMove*
    process_move(move.get()->player_id, move.get()->x, ...);
}
```

**sdk::VarData:**
```cpp
auto msg = sdk::VarData::deserialize(payload);
if (!msg) return PROPAGATION_REJECT;

std::string_view text = msg.get<std::string_view>(0);
int32_t timestamp = msg.get<int32_t>(1);
```

---

**См. также:** [Handler гайд](../guides/handler-guide.md) · [Connector гайд](../guides/connector-guide.md) · [SDK data types](../architecture/plugin-system.md)
