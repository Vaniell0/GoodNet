# 16 — Roadmap

---

## Beta

| Feature | Description |
|---|---|
| **UDP Connector** | Unreliable transport, QUIC-like flow control |
| **WebSocket Connector** | Works through HTTP proxies and from browsers (WASM) |
| **NAT Traversal** | STUN/TURN for connections through deep NAT |
| **Mesh routing** | Packet forwarding through intermediate nodes |
| **Rate limiting** | Built-in DoS protection at core level |
| **Hot reload** | Plugin reload without core restart |
| **Prometheus metrics** | Connection and packet statistics export |
| **Session resumption** | Restore session without re-doing ECDH |

---

## v1.0

| Feature | Description |
|---|---|
| **WASM plugins** | Sandbox for untrusted code |
| **Rust SDK** | Safe bindings via C ABI |
| **Python SDK** | For rapid handler prototyping |
| **Multi-path** | Single logical connection over multiple transports |
| **Perfect Forward Secrecy** | Ephemeral signing + periodic key rotation |

---

*← [15 — Security](15-security.md) · [README →](README.md)*
