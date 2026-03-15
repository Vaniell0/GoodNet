# GoodNet — Документация

> Версия: **0.1.0-alpha** · C++23 · Nix Flakes · libsodium · Boost.Asio · zstd

---

## Содержание

### Часть I — Старт

| # | Файл | Тема |
|---|---|---|
| 01 | [01-overview.md](01-overview.md) | Философия, производительность, quick start, терминология |
| 02 | [02-build.md](02-build.md) | Nix, CMake, CLI флаги, env vars, CoreConfig |

### Часть II — Разработка

| # | Файл | Тема |
|---|---|---|
| 03 | [03-core-api.md](03-core-api.md) | gn::Core, send/subscribe/connect, stats, C API |
| 04 | [04-plugins.md](04-plugins.md) | Handler + Connector SDK (C и C++), lifecycle, SHA-256 |
| 05 | [05-system-messages.md](05-system-messages.md) | AUTH, KEY_EXCHANGE, HEARTBEAT, RELAY, ICE_SIGNAL |
| 06 | [06-usage-patterns.md](06-usage-patterns.md) | Server, client, relay mesh, ICE, echo handler, мониторинг |

### Часть III — Архитектура

| # | Файл | Тема |
|---|---|---|
| 07 | [07-architecture.md](07-architecture.md) | Pimpl, CM, PM, SignalBus, потоки данных, структура репо |
| 08 | [08-protocol.md](08-protocol.md) | Wire header v2, handshake FSM, ECDH, шифрование, identity |
| 09 | [09-security.md](09-security.md) | Модель угроз, зоны доверия, криптопримитивы, ограничения |
| 10 | [10-config.md](10-config.md) | JSON config store, Logger, макросы логирования |

### Часть IV — Справочник

| # | Файл | Тема |
|---|---|---|
| 11 | [11-testing.md](11-testing.md) | GTest suite, mock plugins, coverage, как добавить тест |
| 12 | [12-roadmap.md](12-roadmap.md) | Alpha done, Beta planned, v1.0 vision |

---

## Быстрый старт

```bash
nix develop      # dev окружение
cfg && b         # Release build
nix run .#test   # запустить тесты
nix build        # core + плагины
```

```cpp
#include <core.hpp>

gn::CoreConfig cfg;
cfg.plugins.dirs = {"./result/plugins"};

gn::Core core(cfg);
core.run_async();
core.subscribe(1000, "my_app",
    [](auto, auto hdr, auto ep, auto data) {
        return PROPAGATION_CONSUMED;
    });
core.send("tcp://peer:25565", 1000, "hello", 5);
core.stop();
```

## Производительность

```
XSalsa20-Poly1305 + Zstd L1:
  4 потока / ~30 МБ    → ~10 Гбит/с
  5 потоков / ~300 МБ  → 15–17 Гбит/с
  предел               → ~19 Гбит/с (CPU bound)
```
