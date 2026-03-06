# 04 — Cryptography

All primitives are provided by **libsodium**. No custom-implemented algorithms.

---

## Primitives

| Operation | libsodium primitive | Key/output size |
|---|---|---|
| AUTH signing, header signing | `crypto_sign_ed25519` | sk: 64 B, pk: 32 B, sig: 64 B |
| ECDH key exchange | `crypto_scalarmult` (X25519) | shared: 32 B |
| KDF on top of ECDH | `crypto_generichash` (BLAKE2b-256) | 32 B |
| Packet encryption | `crypto_secretbox_easy` (XSalsa20-Poly1305) | MAC: 16 B |
| Hardware ID hash | `crypto_generichash` (BLAKE2b-256) | 32 B |
| device_key seed | `crypto_generichash` (BLAKE2b-256) | 32 B |
| Manifest SHA-256 | `crypto_hash_sha256` | 32 B |
| Base64 → binary | `sodium_base642bin` | — |
| Secure zeroing | `sodium_memzero` | — |
| Random bytes | `randombytes_buf` | — |

---

## Key Pairs

### user_key (Ed25519)

User identifier. Portable across machines. Can be an SSH key.

```
Sources (priority order):
  1. config: "identity.ssh_key_path"
  2. ~/.ssh/id_ed25519  (HOME / USERPROFILE on Windows)
  3. Generate → <dir>/user_key  (64 raw bytes, chmod 0600)

Used for:
  • Signing AUTH packets         (user_seckey)
  • Peer identification          (user_pubkey in endpoint_t)
  • device_key domain separator  (user_pubkey)
```

### device_key (Ed25519)

Bound to specific hardware. Deterministically reproducible.

```
seed       = BLAKE2b-256(machine_id ‖ user_pubkey)
device_key = crypto_sign_seed_keypair(seed)
sodium_memzero(seed)

Used for:
  • Signing packet headers in ESTABLISHED state  (device_seckey)
  • api->sign_with_device() for plugins
```

Why two keys? `user_key` = "who you are", `device_key` = "which machine". A compromised `device_key` (e.g., leaked VM) does not reveal `user_key`.

### ephem_key (X25519)

One-time use. Generated for each new connection.

```cpp
// cm_handshake.cpp: handle_connect()
rec.session = std::make_unique<SessionState>();
crypto_box_keypair(rec.session->my_ephem_pk,
                   rec.session->my_ephem_sk);

// After derive_session():
sess.clear_ephemeral();  // sodium_memzero(sk) + sodium_memzero(pk)
```

---

## Session Key Derivation

```cpp
// cm_session.cpp: derive_session()

// 1. ECDH
uint8_t shared[32];
crypto_scalarmult(shared, sess.my_ephem_sk, peer_ephem_pk);

// 2. Domain separation: sort user_pk lexicographically
const uint8_t* pk_a = identity_.user_pubkey;
const uint8_t* pk_b = peer_user_pk;
if (std::memcmp(pk_a, pk_b, 32) > 0) std::swap(pk_a, pk_b);

// 3. KDF: BLAKE2b-256(shared ‖ pk_min ‖ pk_max)
crypto_generichash_state st;
crypto_generichash_init(&st, nullptr, 0, 32);
crypto_generichash_update(&st, shared, 32);
crypto_generichash_update(&st, pk_a,   32);
crypto_generichash_update(&st, pk_b,   32);
crypto_generichash_final(&st, sess.session_key, 32);

// 4. Zero intermediate data
sodium_memzero(shared, sizeof(shared));
sess.clear_ephemeral();
sess.ready = true;
```

---

## Encryption / Decryption

### Sending

```cpp
// SessionState::encrypt(plain, plain_len)
const uint64_t n = send_nonce.fetch_add(1, std::memory_order_relaxed);

uint8_t nonce24[24] = {};
for (int i = 0; i < 8; ++i)
    nonce24[i] = static_cast<uint8_t>(n >> (i * 8));  // little-endian

// wire = nonce_prefix[8] ‖ secretbox(plain)
std::vector<uint8_t> wire(8 + plain_len + crypto_secretbox_MACBYTES);
std::memcpy(wire.data(), nonce24, 8);
crypto_secretbox_easy(wire.data() + 8, plain, plain_len, nonce24, session_key);
```

### Receiving

```cpp
// SessionState::decrypt(wire, wire_len)
uint64_t n = 0;
for (int i = 0; i < 8; ++i)
    n |= static_cast<uint64_t>(wire[i]) << (i * 8);

// Replay protection
const uint64_t exp = recv_nonce_expected.load(std::memory_order_acquire);
if (n < exp) {
    LOG_WARN("decrypt: replay nonce={} expected={}", n, exp);
    return {};
}
recv_nonce_expected.store(n + 1, std::memory_order_release);

// Decrypt + verify MAC
if (crypto_secretbox_open_easy(plain.data(), wire + 8,
        wire_len - 8, nonce24, session_key) != 0) {
    LOG_WARN("decrypt: MAC failed nonce={}", n);
    return {};
}
```

### Overhead

```
8 bytes   nonce prefix (uint64_t LE)
16 bytes  Poly1305 MAC
──────────
24 bytes  total overhead per encrypted packet
```

---

## OpenSSH Ed25519 Parser

`cm_identity.cpp: try_load_ssh_key()`

Only unencrypted (`cipher = "none"`) Ed25519 OpenSSH keys are supported.

### Base64 Decoding

```cpp
// cm_identity.cpp
static std::vector<uint8_t> base64_decode(std::string_view in) {
    std::vector<uint8_t> out(in.size()); // upper bound: 3/4 of b64 length
    size_t bin_len = 0;
    if (sodium_base642bin(out.data(), out.size(),
                          in.data(),  in.size(),
                          nullptr,    &bin_len,
                          nullptr,    sodium_base64_VARIANT_ORIGINAL) != 0)
        return {};
    out.resize(bin_len);
    return out;
}
```

`sodium_base64_VARIANT_ORIGINAL` = RFC 4648 standard Base64, used in OpenSSH.
`sodium_base642bin` correctly handles whitespace (PEM line breaks) and padding.

---

## Secret Memory Management

| Data | When zeroed |
|---|---|
| `ephem_sk` | Immediately after `derive_session()` |
| `ephem_pk` | Immediately after `derive_session()` |
| ECDH `shared` | Immediately after BLAKE2b final |
| `device_key seed` | Immediately after `crypto_sign_seed_keypair()` |
| `session_key` | In `~SessionState()` destructor |

`sodium_memzero` guarantees the compiler cannot optimize away the zeroing (unlike `memset`, which can be eliminated as a dead store).

---

## Alpha Limitations

**No strict PFS.** If `user_seckey` is compromised retroactively and all traffic was recorded, the captured AUTH exchange allows recovering `session_key`. True PFS requires ephemeral signing.

**No double ratchet.** Session key is fixed for the lifetime of the TCP connection.

---

*← [03 — Protocol](03-protocol.md) · [05 — ConnectionManager →](05-connection-manager.md)*
