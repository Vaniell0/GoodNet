# ConnectionManager

Самый большой компонент ядра (~2000 строк в `core/cm_*.cpp`). Управляет жизненным циклом соединений, шифрованием, framing, dispatch и relay.

См. также: [Обзор архитектуры](data/projects/GoodNet/docs/architecture.md) · [Wire format](../protocol/wire-format.md) · [Noise_XX handshake](../protocol/noise-handshake.md) · [SignalBus](data/projects/GoodNet/docs/architecture/signal-bus.md)

## RCU registry

Проблема: dispatch path (on_data → handle_data → dispatch_packet) выполняется на IO-потоках и должен быть максимально быстрым. Классический `shared_mutex` создаёт contention при большом количестве параллельных чтений.

Решение — RCU (Read-Copy-Update):

```cpp
using RecordMap    = unordered_map<conn_id_t, shared_ptr<ConnectionRecord>>;
using RecordMapPtr = shared_ptr<const RecordMap>;

atomic<RecordMapPtr> records_rcu_;   // readers: один atomic load
mutex records_write_mu_;              // writers: copy-and-swap
```

**Чтение** (hot path, вызывается на каждый входящий пакет):
```cpp
auto map = records_rcu_.load(memory_order_acquire);
auto it = map->find(id);
// Готово. Без мьютекса. Один atomic load.
```

**Запись** (connect/disconnect — редко):
```cpp
lock_guard lk(records_write_mu_);
auto old  = records_rcu_.load();
auto next = make_shared<RecordMap>(*old);  // полная копия
next->insert_or_assign(id, new_record);
records_rcu_.store(move(next));            // atomic swap
```

Цена: O(N) копирование при каждом connect/disconnect. Но это происходит редко (десятки раз в секунду), а чтение — миллионы раз в секунду. Компромисс оправдан.

### RCU edge case: concurrent writers

Что если два потока одновременно пытаются connect?

```
Время  Thread A (connect peer1)        Thread B (connect peer2)
─────  ──────────────────────────────  ──────────────────────────────
  t0   lock(records_write_mu_)         [ждёт mutex]
  t1   old = records_rcu_.load()
  t2   next = make_shared(*old)
  t3   next->insert(peer1, rec1)
  t4   records_rcu_.store(next)
  t5   unlock(records_write_mu_)
  t6                                    lock(records_write_mu_)  ✅
  t7                                    old = records_rcu_.load()  ← видит peer1!
  t8                                    next = make_shared(*old)
  t9                                    next->insert(peer2, rec2)
  t10                                   records_rcu_.store(next)
  t11                                   unlock(records_write_mu_)
```

**Результат**: Оба peer1 и peer2 присутствуют в финальной карте. Mutex `records_write_mu_` сериализует writers → **нет race condition**.

**Gotcha**: Если бы не было mutex, Thread B мог бы загрузить stale карту (без peer1) → insert peer2 → CAS → peer1 потерян. Mutex критичен!

### RCU snapshot consistency

Readers видят **consistent snapshot** карты. Между `load()` и `find(id)` карта может быть заменена writer'ом, но reader продолжает работать со старой версией (shared_ptr keep-alive):

```
Reader thread:                 Writer thread:
  auto map = load();  ──────►  [map v1 с 10 peers]
  ... some delay ...
                               lock + copy + insert peer11
                               store(map v2)  ──────► [map v2 с 11 peers]
  auto it = map->find(5); ───► [читает map v1 с 10 peers]  ✅ consistent
```

Reader не видит peer11 до следующего `load()`, но это OK — eventual consistency.

## ConnectionRecord

Каждое соединение хранит:
```
conn_id_t id
conn_state_t state                   ← FSM
endpoint_t remote                    ← IP:port
bool is_localhost                    ← EP_FLAG_TRUSTED
bool is_initiator                   ← true если мы инициировали соединение
bool peer_authenticated              ← после завершения Noise_XX
uint8_t peer_user_pubkey[32]         ← Ed25519 user key пира
uint8_t peer_device_pubkey[32]       ← Ed25519 device key пира
unique_ptr<NoiseSession> session     ← transport keys после ESTABLISHED
unique_ptr<noise::HandshakeState> handshake ← активен до ESTABLISHED
string negotiated_scheme             ← "tcp" / "ice"
vector<uint8_t> recv_buf             ← буфер для reassembly
atomic<uint64_t> send_packet_id      ← монотонный счётчик пакетов
atomic<uint64_t> last_heartbeat_recv ← для timeout detection
atomic<uint32_t> missed_heartbeats   ← сброс при PONG
```

## Connection FSM

```
[new connection] → STATE_CONNECTING
  → STATE_NOISE_HANDSHAKE
    → NOISE_INIT → NOISE_RESP → NOISE_FIN
      → STATE_ESTABLISHED ← encrypted traffic flows
        → STATE_CLOSING (graceful drain)
          → STATE_CLOSED
```

[Noise_XX](../protocol/noise-handshake.md) — 3-message handshake. Initiator (`EP_FLAG_OUTBOUND`) отправляет NOISE_INIT (ephemeral key), responder отвечает NOISE_RESP (ephemeral + encrypted static + HandshakePayload), initiator завершает NOISE_FIN (encrypted static + HandshakePayload). После FIN обе стороны вызывают `split()` → два `CipherState` (send/recv) → STATE_ESTABLISHED.

## Dispatch path

Как пакет проходит через систему:

```
TCP socket → read() → on_data(id, raw_bytes, size)
  │
  ▼
handle_data(id, raw, size)  [cm_dispatch.cpp]
  │
  ├─ Fast path: recv_buf пуст + пришёл ровно один полный фрейм
  │   → zero-copy: dispatch_packet() прямо из сырого буфера
  │     (нет memcpy, нет аллокации)
  │
  └─ Slow path: данные неполные или уже есть остаток в recv_buf
      → append к recv_buf → цикл reassembly:
        ├─ bad magic → close_now(id) ← предотвращает DoS
        ├─ неполный фрейм → break, ждём следующий read()
        └─ полный фрейм → dispatch_packet()
  │
  ▼
dispatch_packet(id, header, payload, recv_timestamp)
  │
  ├─ NOISE_INIT (type=1) → handle_noise_init() → STATE_NOISE_HANDSHAKE
  ├─ NOISE_RESP (type=2) → handle_noise_resp() → STATE_NOISE_HANDSHAKE
  ├─ NOISE_FIN (type=3) → handle_noise_fin() → STATE_ESTABLISHED
  │
  ├─ TRUSTED validation:
  │   ├─ GNET_FLAG_TRUSTED + is_localhost → plaintext OK
  │   ├─ GNET_FLAG_TRUSTED + !is_localhost → DROP (спуфинг)
  │   └─ !TRUSTED → decrypt → AEAD verify
  │
  ├─ HEARTBEAT (type=4) → handle_heartbeat() ← core-level, не попадает в SignalBus
  ├─ RELAY (type=10) → handle_relay() → local delivery или forward
  │
  └─ User message → SignalBus → priority-ordered handler chain
      └─ CONSUMED → pin affinity (следующие пакеты этого conn → тот же handler)
```

## Send path

Как пакет отправляется:

```
send(uri, msg_type, payload)
  │
  ├─ resolve_uri(uri) → conn_id (shared_mutex на uri_index_)
  ├─ rcu_find(conn_id) → ConnectionRecord
  │
  ├─ Проверки:
  │   ├─ shutting_down_? → return
  │   ├─ state != ESTABLISHED? → return false
  │   └─ global_pending + payload > 512 MB? → backpressure
  │
  ├─ Если payload > 2 MB → разбить на CHUNK_SIZE (1 MB) фрагменты
  │
  ▼
  send_frame(id, msg_type, payload_chunk)
    │
    ├─ build_frame():
    │   ├─ localhost? → GNET_FLAG_TRUSTED, нет шифрования
    │   └─ !localhost → session->encrypt(payload, pkt_id, ...)
    │       ├─ payload > 512 bytes? → zstd(payload, level=1)
    │       └─ pkt_id = send_packet_id.fetch_add(1)
    │       └─ ChaChaPoly-IETF AEAD с nonce из pkt_id
    │
    ├─ PerConnQueue::try_push(frame)
    │   ├─ fetch_add(frame.size()) — резервирование
    │   ├─ > 8 MB? → fetch_sub() rollback → DROP
    │   └─ OK → lock + push
    │
    └─ flush_queue(id) → drain_batch(64 frames)
        │
        └─ flush_frames_to_connector(id, ops, batch)
            ├─ ops->send_gather? → writev() — один syscall
            └─ fallback → ops->send_to() в цикле
```

### Backpressure strategy flowchart

```
┌────────────────────────────────────────────────────────────────┐
│ Application отправляет пакеты быстрее, чем connector успевает  │
└────────────┬───────────────────────────────────────────────────┘
             ▼
    ┌────────────────────────┐
    │ PerConnQueue::try_push │
    │ pending_bytes += size  │
    └────────┬───────────────┘
             │
             ▼
    pending_bytes > 8 MB?
         │         │
         NO        YES
         │         │
         │         ▼
         │    ┌────────────────────────────────────┐
         │    │ REJECT: fetch_sub (rollback)       │
         │    │ DropReason::Backpressure           │
         │    │ stats_.backpressure.fetch_add(1)   │
         │    └─────────┬──────────────────────────┘
         │              │
         ▼              ▼
    ┌────────────────────────────┐
    │ Пакет сброшен (НЕ queued)  │
    └────────────────────────────┘
         │
         ▼
┌──────────────────────────────────────────────────────────────┐
│ Application должна:                                          │
├──────────────────────────────────────────────────────────────┤
│ (a) Замедлить send rate (rate limiting)                      │
│ (b) Batch мелкие отправки в крупные (reduce overhead)        │
│ (c) Ждать drain:                                             │
│     while (!send(...)) {                                     │
│         std::this_thread::sleep_for(10ms);                   │
│     }                                                         │
└──────────────────────────────────────────────────────────────┘
```

**Почему DROP, а не queue?**

Альтернатива: неограниченная очередь → OOM при медленном connector. Лимит 8 MB + DROP защищает от memory exhaustion, но требует от application корректной обработки send failures.

**Мониторинг backpressure:**

```cpp
auto stats = core.get_stats_snapshot();
if (stats.backpressure > 0) {
    LOG_WARN("Backpressure triggered {} times — slow down send rate!",
             stats.backpressure);
}
```

## Heartbeat

Обнаружение мёртвых соединений. Работает только на ESTABLISHED.

```
Каждые 30 секунд (steady_timer из Core):
  for each ESTABLISHED connection:
    ├─ last_heartbeat_recv == 0 → первый цикл, инициализация
    ├─ elapsed > 30s:
    │   missed++
    │   ├─ missed >= 3 → disconnect(id)  ← мёртвое соединение
    │   └─ missed < 3 → send_heartbeat(id)
    └─ elapsed <= 30s → ничего

HeartbeatPayload (16 bytes, packed):
  timestamp_us [8]  — Unix microseconds
  seq          [4]  — монотонный счётчик
  flags        [1]  — 0x00=PING, 0x01=PONG
  _pad         [3]

PING → эхо timestamp/seq обратно как PONG
PONG → сброс missed_heartbeats, обновление last_heartbeat_recv
```

## Gossip relay

Multi-hop forwarding для пакетов к узлам, с которыми нет прямого соединения.

```
relay_payload = RelayPayload(33 bytes) + inner_frame
  RelayPayload:
    ttl           [1]   — декремент на каждом хопе
    dest_pubkey  [32]   — Ed25519 pubkey получателя

handle_relay():
  1. TTL == 0? → drop
  2. Dedup: hash(packet_id) ^ (payload_type << 32) → O(1) hash set с 30s TTL
  3. dest == my_pubkey? → local delivery (re-enter dispatch_packet)
  4. Иначе → forward:
     a. Прямое соединение с dest (pk_index_)? → send только ему
     b. Нет → gossip broadcast всем ESTABLISHED peers (кроме отправителя)
```

---

**См. также:** [Обзор архитектуры](data/projects/GoodNet/docs/architecture.md) · [Wire format](../protocol/wire-format.md) · [Noise_XX handshake](../protocol/noise-handshake.md) · [SignalBus](data/projects/GoodNet/docs/architecture/signal-bus.md) · [Криптография](../protocol/crypto.md)

## Performance optimizations

### Fast path: zero-copy dispatch

**Условия fast path:**
1. `recv_buf` пуст (нет предыдущих неполных фреймов)
2. Пришёл ровно один полный фрейм (`size == sizeof(header_t) + payload_len`)
3. Валидный header (magic + proto_ver)

**Код (из `cm_dispatch.cpp`):**
```cpp
// Fast path: complete frame, no buffered residue — zero-copy dispatch
if (rec->recv_buf.empty() && size >= sizeof(header_t)) {
    const auto* hdr = reinterpret_cast<const header_t*>(raw);
    const size_t total = sizeof(header_t) + hdr->payload_len;

    if (size == total
        && hdr->magic == GNET_MAGIC
        && hdr->proto_ver == GNET_PROTO_VER)
    {
        const std::span<const uint8_t> payload(
            static_cast<const uint8_t*>(raw) + sizeof(header_t),
            hdr->payload_len);
        dispatch_packet(id, hdr, payload, recv_ts);  // zero-copy!
        return;
    }
}
```

**Gain:**
- Нет `memcpy` (payload передаётся как `std::span` указателя)
- Нет аллокации (`recv_buf` не используется)
- ~40% быстрее для TCP loopback (single full frames)

### Slow path: reassembly

**Когда активируется:**
- TCP stream fragmentation (packet split across multiple reads)
- Множественные фреймы в одном read (batching)

**Логика:**
```cpp
// Append to recv_buf
auto& buf = rec->recv_buf;
buf.insert(buf.end(), bytes, bytes + size);

// MAX_RECV_BUF protection (M2 fix: DoS prevention)
if (buf.size() > MAX_RECV_BUF) {  // 16 MB
    LOG_WARN("recv_buf overflow — closing");
    close_now(id);
    return;
}

// Reassembly loop
while (avail >= sizeof(header_t)) {
    const auto* hdr = reinterpret_cast<const header_t*>(buf.data() + consumed);
    
    // Bad magic → immediate close (DoS protection)
    if (hdr->magic != GNET_MAGIC) {
        close_now(id);
        return;
    }
    
    const size_t total = sizeof(header_t) + hdr->payload_len;
    if (avail < total) break;  // неполный фрейм
    
    // Copy to thread_local buffer → dispatch
    pkt_buf.assign(buf.data() + consumed, buf.data() + consumed + total);
    dispatch_packet(...);
    consumed += total;
}
```

**DoS protection:**
1. **MAX_RECV_BUF (16 MB)**: предотвращает OOM если peer отправляет garbage без валидных фреймов
2. **Bad magic → close_now**: немедленный disconnect при первом невалидном header
3. **Thread-local pkt_buf**: переиспользуется между пакетами (zero alloc после warmup)

### DispatchGuard: clean shutdown

**Проблема M5:** При shutdown Core может вызвать `pm->unload_all()` (dlclose) пока dispatch ещё выполняется → use-after-free в handler code.

**Решение (RAII guard):**
```cpp
void dispatch_packet(...) {
    // M5 fix: track in-flight dispatches
    in_flight_dispatches_.fetch_add(1, std::memory_order_relaxed);
    
    struct DispatchGuard {
        std::atomic<uint32_t>& counter;
        ~DispatchGuard() { counter.fetch_sub(1, std::memory_order_relaxed); }
    } guard{in_flight_dispatches_};
    
    // ... dispatch logic ...
}

void shutdown() {
    shutting_down_.store(true);
    
    // Wait-loop до завершения всех dispatches
    while (in_flight_dispatches_.load() > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    // Теперь безопасно: все dispatches завершены
    pm->unload_all();  // dlclose handlers
}
```

**Гарантия:** Handlers не могут быть выгружены пока их код выполняется.

