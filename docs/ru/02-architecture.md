# 02 — Архитектура

## Компонентная диаграмма

```
┌─────────────────────────────────────────────────────────────────────┐
│                          GoodNet Node                               │
│                                                                     │
│  main.cpp ──▶ CLI (ftxui) ──▶ gn::Core (Pimpl)                      │
│                                    │                                │
│  gn_core_create() ─────────────────┘  ← C API (capi.cpp)            │
│                                    │                                │
│  gn::Core::Impl                    │                                │
│  ├── io_context + work_guard           Boost.Asio thread pool       │
│  ├── Logger                            spdlog, меняется до Core()   │
│  ├── Config                            JSON → flat key-value map    │
│  │                                                                  │
│  ├── NodeIdentity                                                   │
│  │   user_key (Ed25519, переносимый / SSH)                          │
│  │   device_key (Ed25519, hardware-bound via MachineId)             │
│  │                                                                  │
│  ├── ConnectionManager                                              │
│  │   ├── records_ : conn_id → ConnectionRecord                      │
│  │   │   ├── SessionState (session_key, nonces, ephem_keypair)      │
│  │   │   ├── recv_buf (TCP reassembly)                              │
│  │   │   ├── peer_pubkeys, peer_schemes, peer_core_meta             │
│  │   │   └── affinity_plugin (CONSUMED pin)                         │
│  │   ├── StatsCollector (lock-free atomic counters)                 │
│  │   └── SignalBus                                                  │
│  │       channels_[msg_type] → PipelineSignal (RCU, по приоритету)  │
│  │       wildcards_          → PipelineSignal                       │
│  │       on_log              → EventSignal (strand, MAX=10000)      │
│  │       on_connection_state → EventSignal                          │
│  │                                                                  │
│  └── PluginManager                                                  │
│      handlers_[name]    → HandlerInfo { DynLib, handler_t*, api_c } │
│      connectors_[scheme]→ ConnectorInfo { DynLib, ops_t*, api_c }   │
│      SHA-256 verify ДО dlopen(RTLD_NOW|RTLD_LOCAL)                  │
│      priority из plugin_info_t → порядок в PipelineSignal           │
└─────────────────────────────────────────────────────────────────────┘
```

---

## Поток входящего пакета

```
TCP/UDP/WS bytes
       │
       ▼  [Connector.so]
api->on_data(conn_id, raw, size)
       │
       ▼  [ConnectionManager::handle_data()]
  recv_buf += raw
  loop: buf >= sizeof(header_t) + payload_len?
    ├── magic != GNET_MAGIC  → clear buf, break
    ├── buf < total          → wait for more bytes
    └── extract full packet
           │
           ▼  [dispatch_packet()]
           ├── MSG_TYPE_AUTH  → process_auth()
           │     verify Ed25519(peer_user_pk)
           │     read peer_schemes + CoreMeta (если kFullSize)
           │     ECDH → session_key → STATE_ESTABLISHED
           │
           └── other types:
                 state != ESTABLISHED  → drop
                 is_localhost          → plaintext
                 else                  → decrypt (XSalsa20-Poly1305)
                                          + Zstd decompress (если >512 байт)
                        │
                        ▼  records_mu_ released here!
                   SignalBus::dispatch_packet(type, hdr, ep, data)
                        │
                   chain-of-responsibility (по убыванию priority):
                   ┌────┴──────────────────────────────────────┐
                   ▼                                           ▼
            channels_[type]                            wildcards_
            handle_message() → on_message_result()     (Logger и т.д.)
            CONSUMED → стоп + affinity_plugin
            REJECT   → дроп
```

---

## Поток исходящего пакета

```
handler или core.send():
  send("tcp://192.168.1.2:25565", MSG_TYPE_CHAT, data, size)
       │
       ▼  [ConnectionManager::send()]
  resolve_uri(...) → conn_id  (uri_index_ lookup)
  state == ESTABLISHED? → иначе: коннектор → async_connect
       │
       ▼  [send_frame(conn_id, type, payload, size)]
  payload_size > 512 && !is_localhost?
    ├── YES → Zstd compress (level 3) + 4-byte orig_size prefix
    └── NO  → без сжатия
  is_localhost?
    ├── YES → plain header + raw payload
    └── NO  → nonce[8] ‖ XSalsa20-Poly1305-secretbox(payload)
  build header_t
  is_localhost?
    ├── YES → skip signature
    └── NO  → Ed25519(device_seckey, header[0..sizeof_without_sig])
  frame = header_bytes ‖ [encrypted/compressed] payload
       │
       ▼  connector_ops_t::send_to(conn_id, frame)
```

---

## Структура репозитория

```
GoodNet/
│
├── cmake/
│   ├── GoodNetConfig.cmake.in
│   └── pch.cmake
│
├── cli/                         # Interactive terminal UI (ftxui)
│   ├── app.cpp
│   ├── cli.hpp
│   ├── commands.cpp             # connect, send, stats, plugins, …
│   └── views.cpp                # TUI виджеты: таблица соединений, лог
│
├── core/                        # → libgoodnet_core.so
│   ├── connectionManager.hpp
│   ├── cm_identity.cpp
│   ├── cm_session.cpp
│   ├── cm_handshake.cpp
│   ├── cm_dispatch.cpp
│   ├── cm_send.cpp
│   ├── pluginManager.hpp
│   ├── pluginManager_core.cpp
│   ├── pluginManager_query.cpp
│   └── data/
│       ├── machine_id.hpp/cpp
│       └── messages.hpp         # CoreMeta, AuthPayload, HeartbeatPayload
│
├── include/                     # Публичные заголовки ядра
│   ├── core.hpp                 # gn::Core + CoreConfig (Pimpl)
│   ├── core.h                   # C API
│   ├── config.hpp / config.cpp
│   ├── dynlib.hpp
│   ├── fmt_extensions.hpp
│   ├── logger.hpp / logger.cpp
│   └── signals.hpp              # PipelineSignal + EventSignal + SignalBus
│
├── sdk/                         # API для плагинов
│   ├── types.h                  # conn_id_t, header_t, propagation_t, …
│   ├── plugin.h                 # host_api_t
│   ├── handler.h                # handler_t
│   ├── connector.h              # connector_ops_t
│   └── cpp/
│       ├── handler.hpp          # IHandler
│       ├── connector.hpp        # IConnector
│       └── data.hpp             # IData, PodData<T>
│
├── plugins/
│   ├── helper.cmake
│   ├── handlers/logger/
│   └── connectors/tcp/
│
├── src/
│   ├── main.cpp
│   ├── core.cpp                 # gn::Core реализация (тяжёлые зависимости)
│   ├── capi.cpp                 # C API обёртка
│   ├── config.cpp
│   ├── logger.cpp
│   └── signals.cpp
│
├── tests/
│   ├── core.cpp                 # gn::Core lifecycle, subscribe, C API
│   ├── conf.cpp
│   ├── plugins.cpp
│   ├── connection_manager.cpp
│   ├── mock_handler.cpp
│   └── mock_connector.cpp
│
├── nix/
├── CMakeLists.txt
└── flake.nix
```

---

## Слои зависимостей

```
               ┌──────────────────────────────────┐
               │   main.cpp + cli/*.cpp           │
               │   (ftxui, boost_program_options) │
               └──────────────┬───────────────────┘
                              │ links
        ┌─────────────────────▼──────────────────────┐
        │          goodnet_core.so                   │
        │                                            │
        │  include/core.hpp  ← публичный C++ API     │
        │  include/core.h    ← публичный C API       │
        │                                            │
        │  Core → CM → SignalBus → PipelineSignal    │
        │       → PluginManager → DynLib             │
        │       → NodeIdentity  → MachineId          │
        │                                            │
        │  Deps: Boost.Asio, libsodium, spdlog,      │
        │        fmt, nlohmann_json, zstd            │
        └─────────────────────┬──────────────────────┘
                              │ dlopen(RTLD_LOCAL)
        ┌─────────────────────▼───────────────────────┐
        │      Plugin.so                              │
        │  handler_init / connector_init              │
        │  sdk/ заголовки только                      │
        └─────────────────────────────────────────────┘
```

---

*← [01 — Обзор](01-overview.md) · [03 — Протокол →](03-protocol.md)*