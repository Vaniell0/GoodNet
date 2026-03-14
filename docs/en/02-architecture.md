# 02 — Architecture

## Component Diagram

```
┌─────────────────────────────────────────────────────────────────────┐
│                          GoodNet Node                               │
│                                                                     │
│  main.cpp ──▶ Config ──▶ Logger                                     │
│      │                                                              │
│      └──────────────▶  ConnectionManager                            │
│                         │                                           │
│                         ├── NodeIdentity                            │
│                         │   user_key (Ed25519)                      │
│                         │   device_key (Ed25519, hardware-bound)    │
│                         │                                           │
│                         ├── records_ : conn_id → ConnectionRecord   │
│                         │   ├── SessionState (session_key, nonces)  │
│                         │   ├── recv_buf (TCP reassembly)           │
│                         │   └── peer_pubkeys, peer_schemes          │
│                         │                                           │
│                         └── SignalBus                               │
│                             channels_[msg_type][handler_name]       │
│                             wildcards_[handler_name]                │
│                             each channel has its own asio::strand   │
│                                   │                                 │
│  ┌────────────────────────────────▼──────────────────────────────┐  │
│  │                      PluginManager                            │  │
│  │  handlers_[name]   → HandlerInfo { DynLib, handler_t*, api }  │  │
│  │  connectors_[scheme]→ ConnectorInfo{ DynLib, ops_t*,  api }   │  │
│  │  dlopen(RTLD_LOCAL) + SHA-256 verify before open              │  │
│  └───────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────┘
```

---

## Incoming Packet Flow

```
TCP/UDP/WS bytes
       │
       ▼  [Connector.so]
api->on_data(conn_id, raw, size)
       │
       ▼  [ConnectionManager::handle_data()]
  fast path: recv_buf empty && complete frame?
    └── YES → dispatch directly (zero-copy)
  slow path: recv_buf += raw
  loop: buf >= sizeof(header_t) + payload_len?
    ├── magic != GNET_MAGIC  → clear buf, break
    ├── buf < total          → wait for more bytes
    └── extract full packet
           │
           ▼  [dispatch_packet()]
           ├── MSG_TYPE_AUTH  → process_auth()
           │     verify Ed25519(peer_user_pk)
           │     ECDH → session_key → STATE_ESTABLISHED
           │
           └── other types:
                 state != ESTABLISHED  → drop
                 is_localhost          → plaintext dispatch
                 else                  → decrypt (XSalsa20-Poly1305)
                        │
                        ▼  records_mu_ released here!
                   SignalBus::emit(msg_type, hdr, ep, data)
                        │
                   ┌────┴──────────────────────────────┐
                   ▼                                   ▼
            channels_[type]                      wildcards_
            Handler::handle_message()            Logger::handle_message()
            (via strand, async)                  (via strand, async)
```

---

## Outgoing Packet Flow

```
Handler code:
  api_->send(ctx, "192.168.1.2:25565", MSG_TYPE_CHAT, data, size)
       │
       ▼  [ConnectionManager::send()]
  resolve_uri("192.168.1.2:25565") → conn_id  (uri_index_ lookup)
  state == ESTABLISHED?  → else drop + LOG_WARN
       │
       ▼  [send_frame(conn_id, type, payload, size)]
  is_localhost?
    ├── YES → plain header + raw payload
    └── NO  → encrypt: nonce[8] ‖ secretbox(payload, nonce24, session_key)
  build header_t: magic, proto_ver, type, payload_len,
                  packet_id (monotonic), timestamp (realtime),
                  sender_id[16] (device_pubkey prefix)
  frame = header_bytes ‖ [encrypted] payload
       │
       ▼  [connector_ops_t::send_to(conn_id, frame)]
  TCP write / UDP sendto / WS send
```

---

## Repository Layout

```
GoodNet/
│
├── cmake/
│   ├── GoodNetConfig.cmake.in   # CMake package config for downstream
│   └── pch.cmake                # apply_pch() — precompiled headers
│
├── core/                        # → libgoodnet_core.so
│   ├── connectionManager.hpp    # All structs + public ConnectionManager API
│   ├── cm_identity.cpp          # NodeIdentity, OpenSSH Ed25519 parser
│   ├── cm_session.cpp           # SessionState: encrypt/decrypt/derive_session
│   ├── cm_handshake.cpp         # handle_connect/send_auth/process_auth/disconnect
│   ├── cm_dispatch.cpp          # handle_data (TCP reassembly), dispatch_packet
│   ├── cm_send.cpp              # send(), send_frame(), negotiate_scheme
│   ├── pluginManager.hpp        # PluginManager + HandlerInfo + ConnectorInfo
│   ├── pluginManager_core.cpp   # load_plugin(), load_all_plugins(), unload_all()
│   ├── pluginManager_query.cpp  # find_*, list_*, enable/disable/unload
│   └── data/
│       ├── machine_id.hpp       # Cross-platform hardware binding
│       └── messages.hpp         # Typed payload structs
│
├── include/                     # Public core headers
│   ├── config.hpp / config.cpp
│   ├── dynlib.hpp               # RAII dlopen/LoadLibrary
│   ├── fmt_extensions.hpp       # fmt::formatter for range types
│   ├── logger.hpp / logger.cpp
│   └── signals.hpp              # Signal<Args...> + SignalBus
│
├── sdk/                         # Public API for plugin developers
│   ├── types.h                  # conn_id_t, header_t, endpoint_t, MSG_TYPE_*
│   ├── plugin.h                 # host_api_t, GN_EXPORT
│   ├── handler.h                # handler_t (C struct)
│   ├── connector.h              # connector_ops_t (C struct)
│   └── cpp/
│       ├── handler.hpp          # IHandler (C++ base class)
│       ├── connector.hpp        # IConnector (C++ base class)
│       ├── data.hpp             # IData, PodData<T>
│       └── plugin.hpp           # HANDLER_PLUGIN / CONNECTOR_PLUGIN
│
├── plugins/
│   ├── helper.cmake
│   ├── handlers/logger/         # Example: wildcard handler
│   └── connectors/tcp/          # TCP connector on Boost.Asio
│
├── src/
│   ├── main.cpp
│   ├── config.cpp
│   └── logger.cpp               # Heavy spdlog includes — only here
│
├── tests/
│   ├── conf.cpp
│   ├── plugins.cpp
│   ├── connection_manager.cpp
│   ├── mock_handler.cpp         # .so for plugin tests
│   └── mock_connector.cpp       # .so for plugin tests
│
├── nix/
│   ├── buildPlugin.nix          # .so + SHA-256 → .json manifest
│   └── mkCppPlugin.nix
│
├── CMakeLists.txt
└── flake.nix
```

---

## Dependency Layers

```
               ┌─────────────┐
               │  main.cpp   │
               └──────┬──────┘
                      │ links
        ┌─────────────▼──────────────────┐
        │      goodnet_core.so           │
        │  ConnectionManager             │
        │  PluginManager  SignalBus      │
        │  Logger  Config  DynLib        │
        │                                │
        │  Deps: Boost.Asio, libsodium,  │
        │        spdlog, fmt, nlohmann   │
        └─────────────┬──────────────────┘
                      │ dlopen(RTLD_LOCAL)
        ┌─────────────▼──────────────────┐
        │      Plugin.so (any)           │
        │  handler_init / connector_init │
        │  needs: sdk/ headers only      │
        │  sees:  core symbols via RTLD  │
        └────────────────────────────────┘
```

`goodnet_core` is built as **SHARED** (not STATIC): one copy of static variables (Logger singleton, `std::call_once` flags) shared between the core and all plugins.

---

*← [01 — Overview](01-overview.md) · [03 — Protocol →](03-protocol.md)*
