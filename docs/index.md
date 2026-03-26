# GoodNet — Документация

**GoodNet — высокопроизводительный зашифрованный mesh-фреймворк для C++23.** Каждый узел верифицирует пиров через Noise_XX handshake; third-party trust не требуется.

Модульный высокопроизводительный сетевой фреймворк на C++23 с Noise_XX шифрованием и плагинной архитектурой.

**Статус:** Alpha 0.1.0 — core функциональность стабильна, API может меняться.

## Ключевые фичи

- **Noise_XX_25519_ChaChaPoly_BLAKE2b handshake** — формализованный протокол с доказанными свойствами безопасности
- **Multi-hop gossip relay** — O(1) дедупликация через hash set, packet_id + payload_type
- **RCU registry** — lock-free dispatch для hot path, миллионы пакетов в секунду
- **Session affinity handlers** — пинят соединение к handler (~30x performance gain для stateful protocols)
- **Zero-copy fast path** — dispatch напрямую из socket buffer при полном фрейме
- **Typed plugin SDK** — C++ abstractions (IHandler, IConnector) + C ABI для других языков
- **SHA-256 plugin verification** — защита от подмены плагинов, manifest-based loading

## I want to...

| Задача | Куда смотреть |
|--------|---------------|
| **Собрать и запустить за 2 минуты** | → [Быстрый старт](./quickstart.md) |
| **Написать handler** | → [Handler: гайд](./guides/handler-guide.md) |
| **Написать connector** | → [Connector: гайд](./guides/connector-guide.md) |
| **Понять routing и propagation** | → [Core Concepts](./guides/concepts.md) |
| **Настроить для production** | → [Config recipes](./recipes/config-recipes.md) |
| **Troubleshoot crypto errors** | → [Криптография: Модель угроз](./protocol/crypto.md#модель-угроз) |
| **Troubleshoot build problems** | → [Build tips](./recipes/build-tips.md) |
| **Понять архитектуру** | → [Обзор архитектуры](./architecture.md) |
| **Увидеть flow диаграммы** | → [Packet flow diagrams](./diagrams/packet-flow.md) |

## Быстрый старт

- **[Быстрый старт](./quickstart.md)** — сборка и запуск за 2 минуты
- [Сборка](./build.md) — Nix, CMake, опции, Docker CI
- [Конфигурация](./config.md) — Config API (typed struct), JSON, все ключи

## Guides

Практические руководства для разработчиков:

- **[Core Concepts](./guides/concepts.md)** — conn_id vs pubkey, endpoint_t, routing, propagation
- **[Best Practices](./guides/best-practices.md)** — performance, security, error handling, testing
- **[Handler: гайд](./guides/handler-guide.md)** — IHandler, dispatch chain, typed payload, real examples
- **[Connector: гайд](./guides/connector-guide.md)** — IConnector, контракт с core, TCP пример

## Architecture

Детали внутренней архитектуры:

- [Обзор архитектуры](./architecture.md) — Core, компоненты, concurrency model
- [ConnectionManager](./architecture/connection-manager.md) — RCU registry, FSM, dispatch/send path
- [SignalBus](./architecture/signal-bus.md) — PipelineSignal, EventSignal, stats
- [Система плагинов](./architecture/plugin-system.md) — PluginManager, SHA-256, lifecycle, C ABI

## Protocol

Протокол и криптография:

- [Wire format](./protocol/wire-format.md) — header_t v3, типы сообщений, framing
- [Noise_XX handshake](./protocol/noise-handshake.md) — 3-msg обмен, cross-verification, rekey
- [Криптография](./protocol/crypto.md) — ключи, AEAD, nonce, forward secrecy, модель угроз

## Recipes

Практические примеры и советы:

- [Config recipes](./recipes/config-recipes.md) — настройки для high-throughput, low-latency, embedded, debug
- [Build tips](./recipes/build-tips.md) — troubleshooting, ccache, incremental builds
- [Identity migration](./recipes/identity-migration.md) — перенос ключей между машинами

## Проект

- [FAQ](./faq.md) — зачем P2P, почему C++, отличия от libp2p, это не Web3
- [Roadmap](./roadmap.md) — статус alpha, планы Beta/v1.0, известные проблемы

---

**API Reference:** см. Doxygen (автогенерация из комментариев в коде)
