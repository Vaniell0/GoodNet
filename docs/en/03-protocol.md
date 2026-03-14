# 03 — Handshake Protocol

GoodNet uses a simplified **Noise_XX** variant — mutual authentication without a central PKI, without certificates, without a CA. Trust is established via out-of-band `user_pubkey` exchange.

---

## Stage Overview

```
1. TCP connect
2. AUTH (both nodes simultaneously) ── Ed25519 signature + ephem_pk
3. ECDH (local, no network)         ── X25519 + BLAKE2b-256 → session_key
4. ESTABLISHED                      ── all traffic XSalsa20-Poly1305
```

---

## Step 1 — AUTH

Immediately after TCP connect, **both** nodes send `MSG_TYPE_AUTH`. Order does not matter — the exchange is symmetric.

### Wire format `gn::msg::AuthPayload`

```
Offset  Size  Field          Description
──────────────────────────────────────────────────────────────
0       32    user_pubkey    Ed25519 user public key
32      32    device_pubkey  Ed25519 device public key
64      64    signature      Ed25519(user_seckey, user_pk‖device_pk‖ephem_pk)
128     32    ephem_pubkey   X25519 ephemeral key for ECDH
──────────────────────────────────────────────────────────────
kBaseSize = 160 bytes   ← old clients without capability negotiation
──────────────────────────────────────────────────────────────
160     1     schemes_count  Number of schemes (0–8)
161     128   schemes[8][16] Schemes: "tcp\0", "ws\0", … (8 slots × 16 bytes)
──────────────────────────────────────────────────────────────
kFullSize = 289 bytes
```

### What is signed

```
to_sign   = user_pubkey[32] ‖ device_pubkey[32] ‖ ephem_pubkey[32]  (96 bytes)
signature = Ed25519(user_seckey, to_sign)
```

Including `ephem_pubkey` in the signature is the key replay attack defense: a captured AUTH packet is useless because `ephem_seckey` has already been zeroed, and reusing `ephem_pubkey` from an old AUTH requires `user_seckey`.

### Receiving side verification

```cpp
// cm_handshake.cpp: process_auth()
uint8_t to_verify[96];
std::memcpy(to_verify,      ap->user_pubkey,   32);
std::memcpy(to_verify + 32, ap->device_pubkey, 32);
std::memcpy(to_verify + 64, ap->ephem_pubkey,  32);

if (crypto_sign_ed25519_verify_detached(
        ap->signature, to_verify, 96, ap->user_pubkey) != 0) {
    LOG_WARN("conn #{}: AUTH signature invalid", id);
    return false;
}
```

### Backward compatibility

Old clients send `payload_len == 160` (kBaseSize). The core checks the size:

```cpp
if (plen >= kFullSize)
    rec.peer_schemes = parse_schemes(ap->schemes, ap->schemes_count);
// else: peer_schemes empty → negotiate_scheme() picks first local scheme
```

---

## Step 2 — ECDH (local, no network)

After AUTH verification, both sides independently compute the same `session_key`:

```
shared      = X25519(my_ephem_sk, peer_ephem_pk)    // 32 bytes
pk_min      = min(my_user_pk, peer_user_pk)          // lexicographic
pk_max      = max(my_user_pk, peer_user_pk)
session_key = BLAKE2b-256(shared ‖ pk_min ‖ pk_max) // 32 bytes
```

**Why sort keys?** Without sorting, node A would compute `BLAKE2b(shared ‖ A_pk ‖ B_pk)` and B would compute `BLAKE2b(shared ‖ B_pk ‖ A_pk)` — different hashes, session fails. Lexicographic sorting makes the result deterministic regardless of connection direction.

After derivation:
```cpp
sodium_memzero(sess.my_ephem_sk, sizeof(sess.my_ephem_sk));
sodium_memzero(sess.my_ephem_pk, sizeof(sess.my_ephem_pk));
sess.ready = true;
rec.state  = STATE_ESTABLISHED;
```

---

## Step 3 — ESTABLISHED

### Packet encryption

```
Wire payload = nonce_u64_le[8] ‖ XSalsa20-Poly1305(plain, nonce24, session_key)

nonce24 = nonce_u64 (LE, 8 bytes) ‖ 0x00[16]
overhead = 8 (nonce prefix) + 16 (Poly1305 MAC) = 24 bytes/packet
```

`send_nonce` is atomically incremented on each send (`fetch_add`, `memory_order_relaxed`).
`recv_nonce_expected` is monotonically increasing — `nonce < expected` → drop (replay attack).

### Header signing

```cpp
// cm_send.cpp: send_frame(), only for non-localhost ESTABLISHED
const size_t body = offsetof(header_t, signature);
crypto_sign_ed25519_detached(
    hdr.signature, nullptr,
    reinterpret_cast<const uint8_t*>(&hdr), body,
    identity_.device_seckey);
```

---

## Capability Negotiation

After receiving `peer_schemes` from AUTH, the core picks the optimal transport:

```cpp
// cm_send.cpp: negotiate_scheme()
for (const auto& prio : scheme_priority_) {
    if (!has_local(prio)) continue;
    if (rec.peer_schemes.empty()) return prio; // old client
    if (has_peer(rec.peer_schemes, prio)) return prio;
}
return local_schemes().empty() ? "tcp" : local_schemes().front();
```

**Example:**
```
Local:     [tcp, ws]
Peer:      [udp, ws, tcp]
Priority:  [tcp, ws, udp, mock]
Result:    "tcp"  ← first from priority supported by both sides
```

---

## Localhost Optimization

Addresses `127.x.x.x`, `::1`, `localhost` → `is_localhost = true`.

| Step | Behavior |
|---|---|
| AUTH | Fully executed |
| ECDH | Fully executed |
| Payload encryption | **Skipped** |
| Header signature | **Skipped** |

Goal: zero crypto overhead when using GoodNet as an IPC bus between processes on the same machine.

---

## `header_t` Wire Format

```
Offset  Size  Field           Description
────────────────────────────────────────────────────────────────────
0       4     magic           0x474E4554 ('GNET') — garbage guard
4       1     proto_ver       Protocol version (current: 1)
5       1     flags           Reserved, always 0
6       2     reserved        Reserved, always 0
8       8     packet_id       Monotonic per-connection counter
16      8     timestamp       Send time, unix microseconds
24      4     payload_type    MSG_TYPE_* constant
28      2     status          STATUS_OK(0) / STATUS_ERROR(1)
30      4     payload_len     Payload byte length after header
34      64    signature       Ed25519(device_sk, header[0..33])
                              Zero bytes until STATE_ESTABLISHED
────────────────────────────────────────────────────────────────────
Total: 98 bytes (#pragma pack(push,1), no padding)
```

### Message types

| Constant | Value | Description |
|---|---|---|
| `MSG_TYPE_SYSTEM` | 0 | Core system messages |
| `MSG_TYPE_AUTH` | 1 | Handshake (always plaintext) |
| `MSG_TYPE_KEY_EXCHANGE` | 2 | Reserved |
| `MSG_TYPE_HEARTBEAT` | 3 | Keepalive ping/pong |
| `MSG_TYPE_CHAT` | 100 | Text messages |
| `MSG_TYPE_FILE` | 200 | File transfer |
| 1000–9999 | — | User-defined plugin types |
| 10000+ | — | Experimental |

---

## Full Handshake Diagram

```
Node A                          Wire                       Node B
  │                                                           │
  │◀── TCP accept ─────────────────────────────────────────── │
  │                                                           │
  │ gen ephem_keypair()                     gen ephem_keypair()
  │                                                           │
  │── MSG_TYPE_AUTH ─────────────────────────────────────────▶│
  │   [user_pk|device_pk|sig|ephem_pk|schemes]                │
  │                                                           │
  │◀─ MSG_TYPE_AUTH ──────────────────────────────────────────│
  │   [user_pk|device_pk|sig|ephem_pk|schemes]                │
  │                                                           │
  │ verify Ed25519(peer_user_pk, sig)   verify Ed25519(...)   │
  │ ECDH session_key                    ECDH session_key      │
  │ STATE → ESTABLISHED                 STATE → ESTABLISHED   │
  │ sodium_memzero(ephem keys)          sodium_memzero(...)   │
  │                                                           │
  │── MSG_TYPE_CHAT ─────────────────────────────────────────▶│
  │   header[signed] ‖ nonce[8] ‖ secretbox(payload)         │
```

---

*← [02 — Architecture](02-architecture.md) · [04 — Cryptography →](04-crypto.md)*
