# 12 — Roadmap

---

## Alpha 0.1.0 (текущее состояние)

| Компонент | Статус |
|---|---|
| Core (Pimpl, lifecycle, **без синглтона**) | Готов |
| ConnectionManager (handshake, encrypt, framing) | Готов |
| PluginManager (SHA-256, dlopen, lifecycle, unload_connector) | Готов |
| SignalBus (PipelineSignal, EventSignal) | Готов |
| TCP Connector (Boost.Asio, scatter-gather) | Готов |
| ICE/DTLS connector (libnice, weak_ptr fix) | Готов |
| Logger Handler (wildcard) | Готов |
| Wire protocol v2 (header_t 44 байт) | Готов |
| XSalsa20-Poly1305 + BLAKE2b KDF | Готов |
| Ed25519 auth + X25519 ECDH | Готов |
| NodeIdentity (user_key + device_key) | Готов |
| Config (JSON flat keys, watchers) | Готов |
| C API (gn_core_*) | Готов |
| C++ SDK (IHandler, IConnector, PodData) | Готов |
| Relay (smart routing + hash dedup O(1)) | Готов |
| Rekey session (двухфазный, PFS) | Готов |
| SystemServiceDispatcher (0x0100-0x0FFF) | Готов |
| RouteTable (decay GC, MAX_HOPS=16) | Готов |
| Wire structs: DHT, Health, RPC, Routing, TUN/TAP | Готов |
| Plugin templates (handler + connector) | Готов |
| Unit tests (~168) | ~75% coverage |
| Nix build (flake, plugins, Docker image) | Готов |

### Исправленные баги (alpha)

| ID | Описание | Статус |
|----|----------|--------|
| C1 | Гонка decrypt nonce (load+store → CAS) | FIXED |
| C2 | ICE UAF raw owner (→ weak_ptr) | FIXED |
| C3 | Rekey nonce reset до подтверждения пиром (→ двухфазный) | FIXED |
| H1 | TCP do_close без notify_disconnect | FIXED |
| H3 | Constructor exception safety (синглтон удалён) | FIXED |
| H4 | Нет unload_connector() | FIXED |
| H5 | Relay dedup O(N) → O(1) hash set | FIXED |
| M1 | HandlerInfo::enabled не atomic | FIXED |

---

## Beta (планируется)

| Фича | Описание |
|---|---|
| **DHT service** | Реализация Kademlia-подобного DHT поверх 0x0100-0x0102 (wire structs готовы) |
| **Health service** | Автоматический keepalive/RTT + метрики (0x0200-0x0202, wire structs готовы) |
| **Distributed RPC** | Request/response с FNV-1a method hash (0x0300-0x0301, wire structs готовы) |
| **TUN/TAP plugin** | Виртуальный сетевой интерфейс (0x0500-0x0501, wire structs готовы) |
| **Hot reload** | Полный цикл PREPARING → ACTIVE → DRAINING → ZOMBIE |
| **recv_buf garbage** | Закрывать соединение при невалидном фрейме (M2) |
| **send_frame TOCTOU** | shared_ptr на connector ops (M3) |
| **ICE shutdown barrier** | Синхронная teardown + promise/future (H2) |
| **UDP Connector** | Ненадёжный транспорт с QUIC-подобным управлением потоком |
| **WebSocket Connector** | HTTP-прокси, браузер (WASM) |
| **Rate limiting** | Встроенная защита DoS на уровне ядра |
| **Session resumption** | Восстановление сессии без повторного ECDH |
| **Prometheus метрики** | Экспорт StatsSnapshot по HTTP |
| **Docker CI** | Полноценные интеграционные тесты в Docker |
| **Store** | SQLite persistence (скелет `store/` уже есть) |

---

## v1.0 (видение)

| Фича | Описание |
|---|---|
| **Perfect Forward Secrecy** | Ephemeral signing + автоматическая ротация |
| **Double ratchet** | Постоянное обновление session_key |
| **Route learning** | Автоматическое объявление маршрутов через ROUTE_ANNOUNCE (0x0400) |
| **WASM плагины** | Песочница для ненадёжного кода |
| **Rust SDK** | Безопасные биндинги через C ABI |
| **Python SDK** | Прототипирование хендлеров |
| **Multi-path** | Одно логическое соединение через несколько транспортов |
| **Certificate pinning** | Доверенные pubkey stores |

---

*← [11 — Тестирование](11-testing.md) · [README →](README.md)*
