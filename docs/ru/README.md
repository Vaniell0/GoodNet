# GoodNet — Документация

> Версия: **0.1.0-alpha** · C++23 · Nix Flakes · libsodium · Boost.Asio

---

## Содержание

| Файл | Что внутри |
|---|---|
| [01-overview.md](01-overview.md) | Философия, цели, ниша, терминология |
| [02-architecture.md](02-architecture.md) | Компонентная диаграмма, потоки данных, структура репозитория |
| [03-protocol.md](03-protocol.md) | Рукопожатие AUTH, ECDH, wire-формат, capability negotiation |
| [04-crypto.md](04-crypto.md) | Примитивы, ключевые пары, сессии, OpenSSH парсер |
| [05-connection-manager.md](05-connection-manager.md) | FSM, ConnectionRecord, индексы, thread safety |
| [06-signal-bus.md](06-signal-bus.md) | Signal\<T\>, SignalBus, strand-изоляция, подписка |
| [07-plugin-manager.md](07-plugin-manager.md) | Загрузка, верификация SHA-256, DynLib, lifecycle |
| [08-identity.md](08-identity.md) | NodeIdentity, SSH-ключи, MachineId по платформам |
| [09-logger.md](09-logger.md) | Meyers singleton, макросы, флаг %Q, плагины |
| [10-config.md](10-config.md) | variant map, иерархические ключи, fs::path, JSON IO |
| [11-sdk-c.md](11-sdk-c.md) | types.h, host_api_t, handler_t, connector_ops_t |
| [12-sdk-cpp.md](12-sdk-cpp.md) | IHandler, IConnector, PodData\<T\>, макросы плагинов |
| [13-build.md](13-build.md) | Nix Flakes, CMake targets, dev shell, добавление плагина |
| [14-testing.md](14-testing.md) | GTest, coverage, mock-объекты |
| [15-security.md](15-security.md) | Модель угроз, защиты, ограничения |
| [16-roadmap.md](16-roadmap.md) | Beta и v1.0 |

---

## Быстрый старт

```bash
nix develop      # dev окружение: cmake, ninja, gdb, ccache
cfg && b         # Release
cfgd && bd       # Debug + coverage
nix build        # core + плагины + bundle
```

```
Node = ConnectionManager + Plugins

Connector.so  → транспорт (TCP / UDP / WS)
Handler.so    → логика   (чат / лог / прокси)
SignalBus     → маршрутизация пакет → хендлер
PluginManager → load + SHA-256 verify + lifecycle
```

---

*English: [../en/README.md](../en/README.md)*
