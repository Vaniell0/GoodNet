# Roadmap

Текущий статус, планы и известные проблемы GoodNet.

См. также: [Обзор архитектуры](data/projects/GoodNet/docs/architecture.md) · [Быстрый старт](data/projects/GoodNet/docs/quickstart.md)

## Alpha 0.1.0 (текущая)

Что работает:
- **Core**: multi-instance, Pimpl, [Config injection](data/projects/GoodNet/docs/config.md), heartbeat timer
- **[ConnectionManager](data/projects/GoodNet/docs/architecture/connection-manager.md)**: RCU registry, [Noise_XX](data/projects/GoodNet/docs/protocol/noise-handshake.md) handshake, [ChaChaPoly-IETF AEAD](data/projects/GoodNet/docs/protocol/crypto.md), per-conn queue с backpressure, TCP reassembly (fast-path zero-copy), heartbeat PING/PONG, gossip relay с O(1) dedup
- **[Плагины](data/projects/GoodNet/docs/architecture/plugin-system.md)**: SHA-256 verified dlopen, static plugins, C ABI + C++ SDK ([IHandler](data/projects/GoodNet/docs/guides/handler-guide.md), [IConnector](data/projects/GoodNet/docs/guides/connector-guide.md))
- **TCP connector**: Boost.Asio, scatter-gather IO (writev), async двухфазное чтение
- **ICE/DTLS connector**: libnice, STUN/TURN, SDP signaling через TCP
- **[SignalBus](data/projects/GoodNet/docs/architecture/signal-bus.md)**: per-type dispatch, wildcard, priority chain (0 = highest), session affinity, atomic stats
- **[Wire protocol v3](data/projects/GoodNet/docs/protocol/wire-format.md)**: 20-byte header, packet_id as AEAD nonce
- **Benchmark binary**: live dashboard, histogram, structured exit codes
- **Тесты**: GTest, mock-плагины
- **[Nix build](data/projects/GoodNet/docs/build.md)** + Release CI (Linux)
- **Docker CI** для ICE (coturn + 3-node mesh)

### Исправленные баги

| ID | Проблема | Решение |
|----|----------|---------|
| C1 | Nonce race в decrypt | CAS (compare_exchange_weak) loop |
| C2 | ICE use-after-free | weak_ptr + enable_shared_from_this |
| H1 | TCP do_close не уведомлял core | notify_disconnect на обоих путях |
| H4 | Нет unload_connector() | Добавлен |
| H5 | Relay dedup O(N) ring buffer | O(1) hash set с TTL |
| M1 | HandlerInfo::enabled не atomic | std::atomic\<bool\> |
| M2 | recv_buf overflow: без лимита → OOM | MAX_RECV_BUF (16 MB), DropReason::RecvBufOverflow |
| M3 | send_frame TOCTOU: connector может быть выгружен | DropReason::ConnectorNotFound, shutting_down_ guard |
| M5 | Shutdown: dlclose до join IO потоков | pm->unload_all() после join, DispatchGuard RAII, in_flight_dispatches_ atomic |

### Известные проблемы

| ID | Описание |
|----|----------|
| H2 | ICE on_shutdown race с GLib event loop (нужен sync teardown) |

## Beta (планируется)

**Целевой timeline:** Q2 2026 (апрель-июнь). Зависит от завершения [H2](https://github.com/GoodNet/goodnet/issues/H2) и реализации SystemServiceDispatcher.

### Основные фичи

#### SystemServiceDispatcher

Обработчик для системных сервисов (type range 0x0100–0x0FFF). Перехватывает пакеты **до** user handlers через priority=0:

```cpp
class SystemServiceDispatcher {
    void handle_message(header_t* hdr, endpoint_t* ep, span<uint8_t> payload) {
        switch (hdr->payload_type) {
            case MSG_TYPE_DHT_PING:      return handle_dht_ping(...);
            case MSG_TYPE_DHT_FIND_NODE: return handle_dht_find_node(...);
            case MSG_TYPE_RPC_REQUEST:   return handle_rpc_request(...);
            case MSG_TYPE_HEALTH_PING:   return handle_health_ping(...);
            // ...
        }
    }
};
```

**DHT (Distributed Hash Table):**
- Kademlia-style: XOR distance metric, k-buckets (k=20)
- Операции: `ping`, `find_node`, `announce` (payload type 0x0100–0x0102)
- Use case: Service discovery без центральных серверов

**RPC (Remote Procedure Call):**
- Request/response pattern (type 0x0300–0x0301)
- Protocol buffers payload (или JSON for simple cases)
- Use case: Межузловые API (query stats, trigger action)

**Health monitoring:**
- Ping/Pong + metrics reporting (type 0x0200–0x0202)
- Aggregate stats: CPU, RAM, latency histogram
- Use case: Cluster health dashboard

**Routing service:**
- Route announce/query (type 0x0400–0x0401)
- Optimize multi-hop paths (shortest path, lowest latency)
- Use case: Smart relay (вместо gossip broadcast)

#### RouteTable: smart relay с decay GC

Вместо [gossip broadcast](data/projects/GoodNet/docs/architecture/connection-manager.md#gossip-relay) всем peers, отправлять только на оптимальный path:

```
Текущая реализация (gossip):
  Alice → Bob → relay всем (5 peers)

Smart relay (RouteTable):
  Alice → Bob → RouteTable::find_path(dest_pk)
              → relay только Charlie (shortest path к dest)
```

**Decay GC:** Маршруты устаревают с TTL (default 300s). Периодический cleanup dead routes.

**Прототип:** `core/route_table.hpp` (partially implemented).

#### TUN/TAP: L3 tunneling

Создать виртуальный network interface → route IP packets через mesh:

```bash
# На узле A:
ip tuntap add mode tun goodnet0
ip addr add 10.99.0.1/24 dev goodnet0
ip link set goodnet0 up

# GoodNet слушает tun0 → encapsulate IP packets → MSG_TYPE_TUN_DATA
# Другие узлы decrypt → write to их tun0 → deliver IP packet
```

**Use case:** VPN-like tunneling, mesh networking (каждый узел = router).

**Scope (Beta):** Basic L3 forwarding, no NAT/firewall (roadmap v1.0+).

#### Store: SQLite persistent storage

Плагины могут сохранять state в SQLite DB:

```cpp
// Plugin API:
host_api->store_put("my_plugin", "key", "value");
auto val = host_api->store_get("my_plugin", "key");  // → "value"
```

**Schema:** `store/schema.sql` (key-value с plugin namespace isolation).

**Use case:** Persistent DHT buckets, RPC results cache, session state across restarts.

#### Hot-reload lifecycle

Полная реализация [PREPARING → ACTIVE → DRAINING → ZOMBIE](data/projects/GoodNet/docs/architecture/plugin-system.md#plugin-lifecycle):

```
Old version:  ACTIVE → disable() → DRAINING (wait in-flight requests) → ZOMBIE → unload()
New version:  load() → PREPARING → enable() → ACTIVE
```

**Graceful migration:** Нет downtime, requests в DRAINING дожидаются completion.

**Beta scope:** Handler hot-reload (connector hot-reload сложнее — требует migration TCP sockets).

## v1.0 (vision)

**Целевой timeline:** Q4 2026 (октябрь-декабрь).

### Стабильность

- **Стабильный [wire protocol](data/projects/GoodNet/docs/protocol/wire-format.md)**: Breaking changes → major version bump (v2.0, v3.0...)
  - `proto_ver` field в header позволяет compatibility check
  - v1.0 nodes reject v2.0 packets (или negotiate fallback)

- **Стабильный [C ABI](data/projects/GoodNet/docs/architecture/plugin-system.md#c-abi-для-не-c-плагинов)**: Backward-compatible extensions
  - vtable versioning: `host_api_t` v1 → v2 (add fields, не изменять offset старых)
  - Plugins compiled for v1.0 ABI работают с v1.x core (x = minor version)

### Production-ready DHT и service discovery

- **DHT persistence**: Kademlia routing table сохраняется в [Store](data/projects/GoodNet/docs/roadmap.md#store-sqlite-persistent-storage)
- **Service registry**: `announce("chat-service", my_pubkey)` → peers могут `find("chat-service")` → list of providers
- **Use case:** Децентрализованный service mesh (как Consul, но без центральных серверов)

### Trust-on-first-use / PKI policy

**Проблема:** [Текущая уязвимость](data/projects/GoodNet/docs/protocol/crypto.md#не-защищает-от) — MitM на **первом** handshake.

**Решение (v1.0):**

#### Option 1: Trust-on-first-use (TOFU)

```
Первое соединение с peer (pubkey=0xAAA):
  1. Noise handshake → успешно
  2. Сохранить pubkey в trusted list (~/.goodnet/known_peers)
  3. Последующие handshake → проверить pubkey matches known

Если pubkey изменился:
  → WARN: "Peer pubkey changed! Possible MitM. Verify out-of-band."
  → Require manual confirmation (как SSH known_hosts)
```

**Плюсы:** Zero config, нет центральных серверов.
**Минусы:** Первый контакт уязвим (как SSH без StrictHostKeyChecking).

#### Option 2: PKI (Certificate transparency log)

```
Centralized trust anchor (или blockchain-based):
  1. User регистрирует (user_pk, device_pk) в registry
  2. Получает signed certificate (CA signature или blockchain proof)
  3. При handshake передаёт certificate в HandshakePayload
  4. Peer проверяет signature → trusted
```

**Плюсы:** MitM невозможен (даже на первом контакте).
**Минусы:** Требует trust anchor (CA или blockchain consensus).

**v1.0 scope:** Оба механизма (TOFU by default, PKI optional via config).

### Cross-platform

- **Darwin (macOS)**: TCP + ICE connectors работают (boost::asio + libnice portable)
- **Embedded Linux** (Raspberry Pi, OpenWrt): Minimal footprint config (см. [config recipes](data/projects/GoodNet/docs/config.md#embedded-low-power))
- **Windows**: TCP connector работает (boost::asio), ICE нужен (libnice + GLib на Windows)

**v1.0 scope:** macOS + embedded Linux stable, Windows experimental.

### Plugin registry / package manager

```bash
# Установить плагин из registry
goodnet plugin install chat-handler

# Registry URLs: goodnet.io/registry или self-hosted
# SHA-256 verification автоматически
```

**Schema:** `name`, `version`, `url`, `sha256`, `dependencies`.

### Traffic padding (anti correlation attack)

**Проблема:** [Correlation attack](data/projects/GoodNet/docs/protocol/crypto.md#не-защищает-от) — observer видит payload_len в header → может вычислить что передаётся (даже если зашифровано).

**Решение:**

#### Constant-length padding

```
Все пакеты → round up to fixed size buckets:
  128 bytes, 512 bytes, 2048 bytes, 8192 bytes

Payload=400 bytes → bucket=512 → pad 112 bytes
```

**Cost:** Bandwidth overhead ~20-30% (зависит от traffic pattern).

#### Dummy traffic

```
Random intervals (Poisson distribution):
  → send MSG_TYPE_DUMMY (drop on receive, не forward)
  → размер random (512–2048 bytes)
```

**Cost:** CPU + bandwidth overhead. Configurable rate (default: 1 dummy/sec per active connection).

**v1.0 scope:** Constant-length padding (configurable buckets), dummy traffic (opt-in via config).

### English documentation

Перевод всех `.md` файлов на английский (или bilingual: Russian + English tabs).

**Rationale:** Wider adoption, international contributors.

---

## Beyond v1.0 (future)

- **Post-quantum crypto**: Noise_XXpsk3 + Kyber768 hybrid KEM
- **Mix networks / onion routing**: Multi-hop encrypted paths (как Tor)
- **Formal verification**: TLA+ specs для Noise handshake FSM, RCU registry
- **WASM plugins**: WebAssembly runtime вместо native `.so` (sandboxing, portability)

---

**См. также:** [Обзор архитектуры](data/projects/GoodNet/docs/architecture.md) · [Быстрый старт](data/projects/GoodNet/docs/quickstart.md) · [Конфигурация](data/projects/GoodNet/docs/config.md)
