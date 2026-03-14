# GoodNet — Документация

> Версия: **0.1.0-alpha** · C++23 · Nix Flakes · libsodium · Boost.Asio · zstd

---

## Содержание

| Файл | Что внутри |
|---|---|
| [01-overview.md](01-overview.md) | Философия, производительность, терминология |
| [02-architecture.md](02-architecture.md) | Core Pimpl, потоки данных, структура репозитория |
| [03-protocol.md](03-protocol.md) | AUTH, ECDH, CoreMeta, wire-формат, capability negotiation |
| [04-crypto.md](04-crypto.md) | XSalsa20-Poly1305, Zstd, ключевые пары, сессии, ротация |
| [05-connection-manager.md](05-connection-manager.md) | StatsCollector, affinity, backpressure, rotate_identity, thread safety |
| [06-signal-bus.md](06-signal-bus.md) | PipelineSignal (RCU), EventSignal (strand), propagation_t |
| [07-plugin-manager.md](07-plugin-manager.md) | SHA-256, plugin_info_t, plugin_state_t, приоритеты |
| [08-identity.md](08-identity.md) | NodeIdentity, SSH-ключи, MachineId по платформам |
| [09-logger.md](09-logger.md) | Meyers singleton, макросы, %Q флаг, плагины |
| [10-config.md](10-config.md) | variant map, иерархические ключи, JSON IO |
| [11-sdk-c.md](11-sdk-c.md) | types.h, propagation_t, plugin_info_t, host_api_t, handler_t |
| [12-sdk-cpp.md](12-sdk-cpp.md) | IHandler, on_result, send_response, IConnector, PodData\<T\> |
| [13-build.md](13-build.md) | Nix Flakes, CMake, src/core.cpp, ccache |
| [14-testing.md](14-testing.md) | GTest, tests/core.cpp, coverage, mock-объекты |
| [15-security.md](15-security.md) | Модель угроз, XSalsa20-Poly1305, CoreMeta backcompat |
| [16-roadmap.md](16-roadmap.md) | Beta и v1.0 |
| [17-core-api.md](17-core-api.md) | gn::Core, CoreConfig, Pimpl, C API (core.h / capi.cpp) |

---

## Быстрый старт

```bash
nix develop      # dev окружение: cmake, ninja, gdb, ccache
cfg && b         # Release
cfgd && bd       # Debug + coverage
nix build        # core + плагины + bundle
```

```cpp
// C++ API
#include "core.hpp"

gn::CoreConfig cfg;
cfg.plugins.dirs = { "plugins/" };
cfg.logging.level = "info";

gn::Core core(cfg);
core.subscribe(MSG_TYPE_CHAT, "my_handler",
    [](auto, auto hdr, auto ep, auto data) {
        fmt::print("from {}:{}\n", ep->address, ep->port);
        return PROPAGATION_CONSUMED;
    });
core.run();  // блокирует до stop()
```

```c
// C API
gn_config_t cfg = { .log_level = "info", .listen_port = 25565 };
gn_core_t* core = gn_core_create(&cfg);
gn_core_run_async(core, 0);
gn_core_send(core, "tcp://10.0.0.1:25565", 100, "hello", 5);
gn_core_stop(core);
gn_core_destroy(core);
```

## Производительность

```
Noise_XX + XSalsa20-Poly1305 + Zstd L3, Docker, разные сети:
  4 потока / ~30 МБ    → ~10 Гбит/с  (случайные данные)
  5 потоков / ~300 МБ  → 15–17 Гбит/с
  предел               → ~19 Гбит/с  (CPU bound)
```

---

!!!УСТАРЕЛ
*English: [../en/README.md](../en/README.md)*
