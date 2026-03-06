# 01 — Project Overview

## What is GoodNet

**GoodNet** is a modular network framework in C++23. Its architectural goal: give engineers raw control over the network stack while completely decoupling transport from business logic. Every running instance is a symmetric **Node**: it simultaneously listens for incoming connections and initiates outgoing ones. The node's role in the topology is determined by which plugins are loaded, not by the core code.

The project is in **alpha**: the core is stable, the plugin ABI is fixed, the TCP transport and base Logger handler work and are covered by tests.

---

## Philosophy

### 1. Node Symmetry

No hard client/server split. Load a routing module — get a router. Load encryption + bridge — get a secure proxy. One binary, different sets of `.so` files.

### 2. Transport/Logic Separation

```
Connector.so   →  "how bytes enter the node"
Handler.so     →  "what happens to them"
```

Changing transport means replacing one `.so`. The handler has no knowledge of sockets or lower-level protocols. It receives an already-decrypted, verified payload via `handle_message()`.

### 3. Security by Default

Every non-localhost connection mandatorily passes:
1. Mutual Ed25519 authentication (AUTH handshake)
2. X25519 ECDH + BLAKE2b-256 KDF → `session_key`
3. XSalsa20-Poly1305 encryption of all traffic

This is not an option — it's part of the core. The only way to bypass it is localhost mode, which is explicitly marked and not intended for external connections.

### 4. Verifiable Plugins

Every `.so` comes with a JSON manifest containing a SHA-256 hash. `PluginManager` verifies the hash **before** `dlopen`. Silently replacing a plugin is impossible.

### 5. Reproducible Builds

Nix Flakes — all developers get a byte-identical build. Dependencies are pinned in `flake.lock`.

---

## Where GoodNet Fits

| Tool | Problem |
|---|---|
| **Boost.Asio** | Raw I/O without protocol, crypto, routing — you write everything |
| **ZeroMQ** | Enforces PUB/SUB/REQ/REP patterns, hides transport, no crypto |
| **POCO** | Heavy, many abstraction layers, C++03 style |
| **gRPC** | Protobuf, HTTP/2, cloud-oriented, complex configuration |
| **GoodNet** | Async I/O + crypto + verified plugins + Nix |

GoodNet is suited for: p2p applications, IoT gateways, mesh networks, secure IPC, educational projects on network protocols.

---

## What GoodNet is Not

- **Not a ready-made messenger** — just the protocol layer
- **Not a ZeroMQ replacement** — no broker, no built-in messaging patterns in the core
- **Not cloud-first** — no built-in service discovery, TLS certificates, or HTTP API

---

## Terminology

| Term | Definition |
|---|---|
| **Node** | A running GoodNet instance (`ConnectionManager` + plugins) |
| **Handler** | Business logic plugin: receives decrypted packets |
| **Connector** | Transport plugin: manages sockets |
| **user_key** | Ed25519 keypair for the user, portable across machines |
| **device_key** | Ed25519 keypair for the device, deterministic from hardware ID |
| **ephem_key** | X25519 one-time keypair for ECDH, zeroed after derivation |
| **session_key** | XSalsa20-Poly1305 key for the current session |
| **strand** | `boost::asio::strand` — serialized executor without mutex |
| **wire** | Bytes on the network: `header_t` ‖ [encrypted] payload |
| **manifest** | `<plugin>.so.json` — metadata + SHA-256 hash of the plugin |
| **conn_id** | `uint64_t` connection descriptor, issued by the core on `on_connect()` |

---

*← [README](README.md) · [02 — Architecture →](02-architecture.md)*
