# Обзор архитектуры

GoodNet состоит из четырёх компонентов внутри Core и двух типов плагинов снаружи.

См. также: [ConnectionManager](./architecture/connection-manager.md) · [SignalBus](./architecture/signal-bus.md) · [Система плагинов](./architecture/plugin-system.md) · **[Диаграммы →](./diagrams/packet-flow.md)**

## Общая картина

```
┌──────────────────────────────────────────────────────────┐
│                      Core (gn::Core)                     │
│                                                          │
│  ┌────────────┐  ┌──────────────────┐  ┌──────────────┐  │
│  │ SignalBus  │  │ ConnectionManager│  │ PluginManager│  │
│  │            │◄─┤                  │  │              │  │
│  │ per-type   │  │ RCU registry     │  │ dlopen/SHA256│  │
│  │ dispatch   │  │ handshake, AEAD  │  │ lifecycle    │  │
│  │ stats      │  │ per-conn queue   │  │              │  │
│  └────────────┘  │ heartbeat, relay │  └──────────────┘  │
│                  └────────┬─────────┘                    │
│  ┌────────────────────────┼───────────────────────────┐  │
│  │          host_api_t vtable (C ABI)                 │  │
│  └───────────┬─────────────┴──────────┬───────────────┘  │
└──────────────┼────────────────────────┼──────────────────┘
               │                        │
     ┌─────────▼──────┐         ┌───────▼────────┐
     │   Connectors   │         │    Handlers    │
     │  (.so plugins) │         │  (.so plugins) │
     │  tcp, ice      │         │  logger, ...   │
     └────────────────┘         └────────────────┘
```

Между Core и плагинами — единственный интерфейс: C ABI vtable (`host_api_t`), который инжектируется в каждый плагин при загрузке.

## Компоненты

### Core

Pimpl-обёртка (`Core::Impl` в `src/core.cpp`). Владеет:
- `boost::asio::io_context` + work guard + пул потоков
- **[SignalBus](./architecture/signal-bus.md)** — маршрутизация событий через priority chain
- **[ConnectionManager](./architecture/connection-manager.md)** — RCU registry, Noise handshake, AEAD, per-conn queue
- **[PluginManager](./architecture/plugin-system.md)** — SHA-256 verified dlopen, lifecycle

Множественные экземпляры Core допускаются (синглтон удалён). [Config](./config.md) инжектируется через указатель.

### Lifecycle

```
Core(Config*) → constructor:
  1. Logger init (level, file, rotation)
  2. NodeIdentity::load_or_generate()
  3. ConnectionManager + PluginManager creation
  4. load_static_plugins() → load .so from base_dir + extra_dirs
  5. Register connectors/handlers

run() →                          ← БЛОКИРУЮЩИЙ
  1. run_async()                  ← запуск потоков
  2. join всех io_threads         ← блокирует до stop()
  3. io_threads.clear()

run_async(threads) →              ← НЕБЛОКИРУЮЩИЙ
  1. heartbeat timer (30s)
  2. N io_threads → ioc->run()
  3. return немедленно             ← управление возвращается вызывающему

stop() →
  1. heartbeat_timer->cancel()
  2. cm->shutdown() (shutting_down_=true, wait in_flight_dispatches_==0)
  3. work_guard.reset()
  4. ioc->stop()
  5. thread join
  6. pm->unload_all() (dlclose безопасен после join)
  7. io_threads.clear()
```

**`run()` vs `run_async()`:** `run()` — блокирующий вызов, внутри вызывает `run_async()` и затем `join()` на всех IO-потоках (возвращает управление только после `stop()`). `run_async()` — неблокирующий, запускает потоки и возвращает управление сразу. Используйте `run_async()` если нужен контроль над основным потоком.

**`reload_config()`:** Горячая перезагрузка конфигурации без перезапуска. Вызывает `Config::reload()` и применяет изменяемые параметры (напр. `Logger::set_log_level()`). Возвращает `false` если reload не удался. Реализация: `src/core.cpp:260-267`.

**Критично:** `pm->unload_all()` **после** join IO-потоков → предотвращает use-after-free.

## Data flow

**Packet-in:** TCP read → framing → decrypt → dispatch → handler chain
**Packet-out:** send() → RCU → encrypt → PerConnQueue → connector

Подробные диаграммы: **[Packet flow →](./diagrams/packet-flow.md)** · **[Noise FSM →](./diagrams/noise-fsm.md)** · **[Connection FSM →](./diagrams/connection-fsm.md)**

## Concurrency model

- **IO потоки**: пул `std::thread`, каждый `ioc->run()`
- **TCP connector**: свой `io_context` + потоки (изоляция)
- **Shared state**:
  - `records_rcu_` — [RCU](./architecture/connection-manager.md#rcu-registry) (atomic + mutex writers)
  - `handlers_mu_`, `connectors_mu_` — shared_mutex
  - `shutting_down_` — atomic\<bool\>

### Синхронизированные структуры CM (core/cm/impl.hpp)

| Mutex / Atomic | Защищаемые данные | Паттерн |
|----------------|-------------------|---------|
| `records_write_mu_` + `records_rcu_` | Реестр соединений (RecordMap) | RCU: atomic read, mutex write |
| `queues_mu_` | `send_queues_` (per-conn outbound queues) | shared_mutex |
| `uri_mu_` | `uri_index_` (URI → conn_id mapping) | shared_mutex |
| `pk_mu_` | `pk_index_` (pubkey_hex → conn_id mapping) | shared_mutex |
| `transport_mu_` | `transport_index_` (transport_conn_id → peer conn_id) | shared_mutex |
| `pending_mu_` | `pending_messages_` (URI → очередь PendingMessage) | shared_mutex |
| `identity_mu_` | `identity_` (NodeIdentity при rotate_identity_keys) | shared_mutex |
| `relay_dedup_mu_` | `relay_dedup_set_` (dedup fingerprints) | mutex |
| `handlers_mu_` | `handler_entries_` (зарегистрированные handlers) | shared_mutex |
| `connectors_mu_` | `connectors_` (scheme → connector_ops_t*) | shared_mutex |
| `in_flight_dispatches_` | Счётчик активных dispatch операций | atomic\<uint32_t\> |
| `shutting_down_` | Флаг остановки | atomic\<bool\> |

**Hot path** (dispatch, send):
- ✅ Atomic loads, RCU reads
- ❌ NO mutable shared state без sync

**Cold path** (connect, disconnect):
- ✅ Shared_mutex для maps
- ✅ Mutex для RCU writers (copy-and-swap)

**Shutdown path:**
- `DispatchGuard` RAII — track `in_flight_dispatches_`
- Wait-loop до `in_flight_dispatches_ == 0`
- Join IO threads → dlclose плагинов

## Файлы

| Путь | Компонент |
|------|-----------|
| `src/core.cpp` | Core lifecycle, plugin wiring, heartbeat |
| `src/signals.cpp` | [SignalBus](./architecture/signal-bus.md), stats |
| `core/cm/dispatch.cpp` | handle_data, dispatch_packet |
| `core/cm/transport.cpp` | build_frame, send_frame, encrypt |
| `core/cm/handshake.cpp` | [Noise_XX](./protocol/noise-handshake.md) handshake |
| `core/cm/lifecycle.cpp` | connect, shutdown, C-ABI trampolines |
| `core/cm/identity.cpp` | NodeIdentity, load_or_generate |
| `core/cm/relay.cpp` | Gossip relay, dedup |
| `core/cm/registration.cpp` | register_connector, register_handler |
| `core/cm/queries.cpp` | find_conn_by_pubkey, get_peer_*, dump |
| `core/cm/disconnect.cpp` | disconnect, close_now |
| `core/cm/impl.hpp` | CM private Pimpl |
| `core/pm/core.cpp` | [PluginManager](./architecture/plugin-system.md) load, unload, lifecycle |
| `core/pm/query.cpp` | get_active_*, counts |
| `core/pm/impl.hpp` | PM private Pimpl |
| `core/crypto/noise.cpp` | HandshakeState, encrypt/decrypt |
| `core/crypto/machine_id.cpp` | Hardware-derived device key |

---

**См. также:** [ConnectionManager детали](./architecture/connection-manager.md) · [SignalBus детали](./architecture/signal-bus.md) · [Диаграммы](./diagrams/packet-flow.md)
