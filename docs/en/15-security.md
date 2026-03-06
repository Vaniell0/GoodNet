# 15 — Security

---

## Threat Model

| Threat | Mitigation |
|---|---|
| MITM during handshake | Ed25519 AUTH signature covers `ephem_pubkey` |
| Replay attack on AUTH | `ephem_pk` is one-time, included in signature |
| Replay attack on packets | Monotonic `recv_nonce_expected` — nonce < expected → drop |
| Packet tampering | Poly1305 MAC — any modification → MAC fail → drop |
| Plugin replacement | SHA-256 against manifest before `dlopen` |
| Single account compromise | `device_key = f(machine_id, user_pk)` — unique per pair |
| Transferring `device_key` to another machine | Bound to hardware ID — not reproducible |
| Ephemeral key leak | `sodium_memzero` immediately after `derive_session()` |
| Session key leak | `sodium_memzero` in `~SessionState()` |
| Pre-auth flood | Packets before STATE_ESTABLISHED are dropped (except AUTH) |
| Symbol conflicts between plugins | `RTLD_LOCAL` isolates each `.so` |

---

## Is localhost mode safe?

Crypto is skipped for `127.x.x.x` / `::1`, but AUTH still runs. Rationale:
- Peer is still identified via Ed25519
- Localhost traffic never leaves the machine — encryption is pointless
- IPC performance is critical for some use cases

Do not use localhost mode for traffic that could be intercepted (container-to-container without network isolation).

---

## Known Limitations (alpha)

### No strict PFS
If `user_seckey` is compromised retroactively + all traffic was recorded → `session_key` can be recovered from captured AUTH. True PFS requires ephemeral signing.

### No double ratchet
Session key is fixed for the TCP connection lifetime.

### No rate limiting in core
Must be implemented in the handler:

```cpp
if (++auth_attempts[remote_ip] > Config::Security::MAX_AUTH_ATTEMPTS)
    disconnect(id);
if (auth_timer.elapsed() > Config::Security::KEY_EXCHANGE_TIMEOUT)
    disconnect(id);
```

### No certificate pinning
No global PKI. Trust via out-of-band `user_pubkey` exchange (QR code, messenger, physical presence).

---

*← [14 — Testing](14-testing.md) · [16 — Roadmap →](16-roadmap.md)*
