# Обзор архитектуры

GoodNet состоит из четырёх компонентов внутри Core и двух типов плагинов снаружи.

См. также: [ConnectionManager](data/projects/GoodNet/docs/architecture/connection-manager.md) · [SignalBus](data/projects/GoodNet/docs/architecture/signal-bus.md) · [Система плагинов](data/projects/GoodNet/docs/architecture/plugin-system.md) · **[Диаграммы →](data/projects/GoodNet/docs/diagrams/packet-flow.md)**

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
- **[SignalBus](data/projects/GoodNet/docs/architecture/signal-bus.md)** — маршрутизация событий через priority chain
- **[ConnectionManager](data/projects/GoodNet/docs/architecture/connection-manager.md)** — RCU registry, Noise handshake, AEAD, per-conn queue
- **[PluginManager](data/projects/GoodNet/docs/architecture/plugin-system.md)** — SHA-256 verified dlopen, lifecycle

Множественные экземпляры Core допускаются (синглтон удалён). [Config](data/projects/GoodNet/docs/config.md) инжектируется через указатель.

### Lifecycle

```
Core(Config*) → constructor:
  1. Logger init
  2. NodeIdentity::load_or_generate()
  3. ConnectionManager + PluginManager creation
  4. Load static plugins → load .so
  5. Register connectors/handlers

run_async(threads) →
  1. heartbeat timer (30s)
  2. N io_threads → ioc->run()

stop() →
  1. cm->shutdown() (shutting_down_=true)
  2. ioc->stop()
  3. thread join
  4. pm->unload_all() (dlclose безопасен после join)
```

**Критично:** `pm->unload_all()` **после** join IO-потоков → предотвращает use-after-free.

## Data flow

**Packet-in:** TCP read → framing → decrypt → dispatch → handler chain
**Packet-out:** send() → RCU → encrypt → PerConnQueue → connector

Подробные диаграммы: **[Packet flow →](data/projects/GoodNet/docs/diagrams/packet-flow.md)** · **[Noise FSM →](data/projects/GoodNet/docs/diagrams/noise-fsm.md)** · **[Connection FSM →](data/projects/GoodNet/docs/diagrams/connection-fsm.md)**

## Concurrency model

- **IO потоки**: пул `std::thread`, каждый `ioc->run()`
- **TCP connector**: свой `io_context` + потоки (изоляция)
- **Shared state**:
  - `records_rcu_` — [RCU](data/projects/GoodNet/docs/architecture/connection-manager.md#rcu-registry) (atomic + mutex writers)
  - `handlers_mu_`, `connectors_mu_` — shared_mutex
  - `shutting_down_` — atomic\<bool\>

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
| `src/signals.cpp` | [SignalBus](data/projects/GoodNet/docs/architecture/signal-bus.md), stats |
| `core/cm_dispatch.cpp` | handle_data, dispatch_packet |
| `core/cm_transport.cpp` | build_frame, send_frame, encrypt |
| `core/cm_handshake.cpp` | [Noise_XX](data/projects/GoodNet/docs/protocol/noise-handshake.md) handshake |
| `core/cm_lifecycle.cpp` | connect, disconnect, shutdown |
| `core/cm_relay.cpp` | Gossip relay, dedup |
| `core/cm_impl.hpp` | CM private Pimpl |
| `core/pluginManager*.cpp` | [PluginManager](data/projects/GoodNet/docs/architecture/plugin-system.md) |
| `core/pm_impl.hpp` | PM private Pimpl |

---

**См. также:** [ConnectionManager детали](data/projects/GoodNet/docs/architecture/connection-manager.md) · [SignalBus детали](data/projects/GoodNet/docs/architecture/signal-bus.md) · [Диаграммы](data/projects/GoodNet/docs/diagrams/packet-flow.md)
