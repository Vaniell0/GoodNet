# 05 — ConnectionManager

`core/connectionManager.hpp` · `core/cm_*.cpp`

Central class. Manages the full connection lifecycle from TCP accept to delivering decrypted payload into SignalBus.

---

## State Machine

```
on_connect() called by connector
        │
        ▼
 ┌──────────────┐     AUTH + ECDH OK
 │ AUTH_PENDING │ ──────────────────────▶ ┌─────────────┐
 └──────────────┘                          │ ESTABLISHED │ ◀─ all traffic
        │                                  └──────┬──────┘
        │ on_disconnect()                         │ on_disconnect()
        ▼                                         ▼
   ┌─────────┐                               ┌─────────┐
   │ CLOSED  │                               │ CLOSED  │
   └─────────┘                               └─────────┘

STATE_CONNECTING   — set by connector before calling api->on_connect()
STATE_KEY_EXCHANGE — reserved (not used in alpha)
STATE_CLOSING      — graceful close (TODO beta)
STATE_BLOCKED      — blocked by policy (TODO)
```

---

## ConnectionRecord

```cpp
struct ConnectionRecord {
    conn_id_t    id;
    conn_state_t state = STATE_AUTH_PENDING;

    endpoint_t   remote;               // IP:port + peer_pubkey (after AUTH)
    std::string  local_scheme;         // scheme on which connection arrived
    std::string  negotiated_scheme;    // selected after capability negotiation

    std::vector<std::string> peer_schemes;   // announced by peer in AUTH

    uint8_t peer_user_pubkey  [32]{};
    uint8_t peer_device_pubkey[32]{};
    bool    peer_authenticated = false;
    bool    is_localhost       = false;   // skip crypto

    std::unique_ptr<SessionState> session; // nullptr before ECDH
    std::vector<uint8_t>          recv_buf; // TCP reassembly buffer
};
```

---

## Indexes

```cpp
std::unordered_map<conn_id_t, ConnectionRecord> records_;
mutable std::shared_mutex records_mu_;

std::unordered_map<std::string, conn_id_t> uri_index_;  // for send(uri,...)
mutable std::shared_mutex uri_mu_;

std::unordered_map<std::string, conn_id_t> pk_index_;   // lookup by peer key
mutable std::shared_mutex pk_mu_;
```

Three separate mutexes — reading `uri_index_` doesn't block writes to `records_`.

---

## Thread Safety

**Critical point in `dispatch_packet`:**

```cpp
// Hold records_mu_ during decrypt:
std::vector<uint8_t> plain;
{
    std::unique_lock lk(records_mu_);
    plain = sess.decrypt(cipher, len);
}  // ← lock released here

// emit called WITHOUT records_mu_:
bus_.emit(type, hdr_ptr, &remote, make_shared<vector>(plain));
// Handler may call api->send() → records_mu_ again
// Without this unlock there would be a deadlock
```

---

## File Breakdown

### `cm_identity.cpp`
NodeIdentity loading, OpenSSH Ed25519 parser with `sodium_base642bin`, keypair generation, `chmod 0600`.

### `cm_session.cpp`
`SessionState::encrypt()`, `decrypt()`, replay protection, `derive_session()` (X25519 + BLAKE2b KDF).

### `cm_handshake.cpp`
Constructor, destructor, `fill_host_api()`, `register_connector/handler()`, `handle_connect()`, `send_auth()`, `process_auth()`, `handle_disconnect()`, C-ABI static adapters.

### `cm_dispatch.cpp`
`handle_data()` — TCP reassembly with magic validation.
`dispatch_packet()` — AUTH routing, decrypt, `bus_.emit()`.

### `cm_send.cpp`
`send()`, `send_frame()`, `negotiate_scheme()`, `resolve_uri()`, `local_schemes()`, getters.

---

## TCP Reassembly

```cpp
// cm_dispatch.cpp: handle_data()
rec.recv_buf.insert(rec.recv_buf.end(), data, data + size);

while (rec.recv_buf.size() >= sizeof(header_t)) {
    const auto* hdr = reinterpret_cast<const header_t*>(rec.recv_buf.data());

    if (hdr->magic != GNET_MAGIC) {
        LOG_WARN("conn #{}: bad magic, clearing recv_buf", id);
        rec.recv_buf.clear(); break;
    }

    const size_t total = sizeof(header_t) + hdr->payload_len;
    if (rec.recv_buf.size() < total) break;  // wait for more

    std::vector<uint8_t> pkt(recv_buf.begin(), recv_buf.begin() + total);
    recv_buf.erase(recv_buf.begin(), recv_buf.begin() + total);

    lk.unlock();
    dispatch_packet(id, pkt);
    lk.lock();
    if (records_.find(id) == records_.end()) break;
}
```

---

*← [04 — Cryptography](04-crypto.md) · [06 — SignalBus →](06-signal-bus.md)*
