# Packet Flow Diagrams

Mermaid блок-схемы для packet-in и packet-out потоков.

См. также: [Обзор архитектуры](../architecture.md) · [ConnectionManager](../architecture/connection-manager.md)

## Packet-in (приём) — упрощённый

```mermaid
flowchart TD
    TCP[TCP Connector<br/>async_read] --> CM[ConnectionManager<br/>handle_data]

    CM --> FRAME{Framing}
    FRAME -->|Fast path<br/>zero-copy| DISPATCH
    FRAME -->|Slow path<br/>reassembly| DISPATCH[dispatch_packet]

    DISPATCH --> TYPE{payload_type?}
    TYPE -->|NOISE_*| HANDSHAKE[Noise handshake<br/>INIT/RESP/FIN]
    TYPE -->|HEARTBEAT| HEARTBEAT[PING/PONG]
    TYPE -->|RELAY| RELAY[Forward/deliver]
    TYPE -->|User msg| DECRYPT

    DECRYPT[AEAD decrypt<br/>ChaChaPoly] --> DECOMPRESS[Decompress<br/>if ZSTD]
    DECOMPRESS --> BUS[SignalBus]

    BUS --> CHAIN{Handler chain}
    CHAIN -->|Session affinity| DIRECT[Direct → pinned handler]
    CHAIN -->|No affinity| PRIORITY[Priority chain<br/>0→255]

    DIRECT --> APP[Application]
    PRIORITY --> APP
```

**Ключевые моменты:**
- **Fast path**: recv_buf пуст + полный фрейм → zero-copy dispatch
- **Slow path**: неполные данные → append recv_buf → reassembly loop
- **AEAD decrypt**: ChaChaPoly-IETF, nonce = 0x00[4] + packet_id[8]
- **Session affinity**: CONSUMED пинит handler → skip chain (~30x faster)

## Packet-out (отправка) — упрощённый

```mermaid
flowchart TD
    APP[Application<br/>send uri, type, data] --> CORE[Core::send]

    CORE --> RESOLVE[resolve_uri<br/>→ conn_id]
    RESOLVE --> RCU[RCU load<br/>records_rcu_]

    RCU --> CHECK{Checks}
    CHECK -->|state ≠ ESTABLISHED| FAIL[return false]
    CHECK -->|pending > 8MB| FAIL
    CHECK -->|OK| BUILD

    BUILD[build_frame] --> CRYPTO{Crypto}
    CRYPTO -->|localhost| PLAIN[TRUSTED flag<br/>plaintext]
    CRYPTO -->|remote| ENCRYPT

    ENCRYPT[ChaChaPoly encrypt] --> COMPRESS{payload > 512B?}
    COMPRESS -->|Yes| ZSTD[zstd level=1<br/>if smaller]
    COMPRESS -->|No| QUEUE
    ZSTD --> QUEUE

    PLAIN --> QUEUE[PerConnQueue<br/>try_push]

    QUEUE --> LIMIT{< 8MB limit?}
    LIMIT -->|No| DROP[DROP<br/>Backpressure]
    LIMIT -->|Yes| FLUSH[flush_queue<br/>batch 64]

    FLUSH --> GATHER{send_gather?}
    GATHER -->|Yes| WRITEV[writev<br/>1 syscall]
    GATHER -->|No| LOOP[loop send_to]

    WRITEV --> NET[Network]
    LOOP --> NET
```

**Ключевые моменты:**
- **RCU**: atomic load → zero lock contention
- **Backpressure**: 8MB per-conn limit → DROP если превышен
- **Compression**: zstd level=1 для payload > 512B (если сжатие выгодно)
- **Batching**: flush до 64 frames за раз → меньше syscalls

## Data structures

```
ConnectionRecord {
  conn_id_t id
  conn_state_t state          // FSM: CONNECTING → HANDSHAKE → ESTABLISHED
  endpoint_t remote           // IP:port
  unique_ptr<NoiseSession>    // send_key, recv_key, handshake_hash
  atomic<uint64_t> send_packet_id
  atomic<uint64_t> recv_nonce_expected
  vector<uint8_t> recv_buf    // reassembly buffer
}

PerConnQueue {
  deque<vector<uint8_t>> frames
  atomic<size_t> pending_bytes  // backpressure tracking
  mutex mu
}
```

---

**См. также:** [ConnectionManager: dispatch path](../architecture/connection-manager.md#dispatch-path) · [ConnectionManager: send path](../architecture/connection-manager.md#send-path)
