# 12 — Roadmap

---

## Alpha 0.1.0 (текущее состояние)

| Компонент | Статус |
|---|---|
| Core (Pimpl, lifecycle, singleton) | Готов |
| ConnectionManager (handshake, encrypt, framing) | Готов |
| PluginManager (SHA-256, dlopen, lifecycle) | Готов |
| SignalBus (PipelineSignal, EventSignal) | Готов |
| TCP Connector (Boost.Asio, scatter-gather) | Готов |
| Logger Handler (wildcard) | Готов |
| Wire protocol v2 (header_t 44 байт) | Готов |
| XSalsa20-Poly1305 + BLAKE2b KDF | Готов |
| Ed25519 auth + X25519 ECDH | Готов |
| NodeIdentity (user_key + device_key) | Готов |
| Config (JSON flat keys, watchers) | Готов |
| C API (gn_core_*) | Готов |
| C++ SDK (IHandler, IConnector, PodData) | Готов |
| ICE/DTLS connector (libnice) | Готов |
| Relay (gossip, TTL, dedup) | Готов |
| Rekey session (PFS) | Готов |
| Unit tests (~168) | ~75% coverage |
| Nix build (flake, plugins, Docker image) | Готов |

---

## Beta (планируется)

| Фича | Описание |
|---|---|
| **Heartbeat** | Автоматический keepalive/RTT из ядра (структура уже определена) |
| **Hot reload** | Полный цикл PREPARING → ACTIVE → DRAINING → ZOMBIE |
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
| **WASM плагины** | Песочница для ненадёжного кода |
| **Rust SDK** | Безопасные биндинги через C ABI |
| **Python SDK** | Прототипирование хендлеров |
| **Multi-path** | Одно логическое соединение через несколько транспортов |
| **Certificate pinning** | Доверенные pubkey stores |

---

*← [11 — Тестирование](11-testing.md) · [README →](README.md)*
