# GoodNet — Documentation(УСТАРЕЛ)

> Version: **0.1.0-alpha** · C++23 · Nix Flakes · libsodium · Boost.Asio

---

## Contents

| File | What's inside |
|---|---|
| [01-overview.md](01-overview.md) | Philosophy, goals, niche, terminology |
| [02-architecture.md](02-architecture.md) | Component diagram, data flows, repository layout |
| [03-protocol.md](03-protocol.md) | AUTH handshake, ECDH, wire format, capability negotiation |
| [04-crypto.md](04-crypto.md) | Primitives, key pairs, sessions, OpenSSH parser |
| [05-connection-manager.md](05-connection-manager.md) | FSM, ConnectionRecord, indexes, thread safety |
| [06-signal-bus.md](06-signal-bus.md) | Signal\<T\>, SignalBus, strand isolation, subscriptions |
| [07-plugin-manager.md](07-plugin-manager.md) | Loading, SHA-256 verification, DynLib, lifecycle |
| [08-identity.md](08-identity.md) | NodeIdentity, SSH keys, MachineId per platform |
| [09-logger.md](09-logger.md) | Meyers singleton, macros, %Q flag, plugin injection |
| [10-config.md](10-config.md) | variant map, hierarchical keys, fs::path, JSON IO |
| [11-sdk-c.md](11-sdk-c.md) | types.h, host_api_t, handler_t, connector_ops_t |
| [12-sdk-cpp.md](12-sdk-cpp.md) | IHandler, IConnector, PodData\<T\>, plugin macros |
| [13-build.md](13-build.md) | Nix Flakes, CMake targets, dev shell, adding a plugin |
| [14-testing.md](14-testing.md) | GTest, coverage, mock objects |
| [15-security.md](15-security.md) | Threat model, mitigations, known limitations |
| [16-roadmap.md](16-roadmap.md) | Beta and v1.0 plans |

---

## Quick Start

```bash
nix develop      # dev env: cmake, ninja, gdb, ccache
cfg && b         # Release build
cfgd && bd       # Debug build + coverage
nix build        # core + all plugins + bundle
```

```
Node = ConnectionManager + Plugins

Connector.so  → transport  (TCP / UDP / WS)
Handler.so    → logic      (chat / logger / proxy)
SignalBus     → routes packets from core to handlers
PluginManager → load + SHA-256 verify + lifecycle
```

---

*Русская версия: [../ru/README.md](../ru/README.md)*
