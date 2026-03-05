# ConnectionManager

## Purpose

`ConnectionManager` is the central component of the GoodNet core, responsible for:

* **Connection Registry Management** – Storing lightweight `ConnectionRecord` entries for every active connection.
* **Identity Management** – Loading and storing the node's Ed25519 keys (`NodeIdentity`).
* **Peer Authorization** – Verifying signatures during connection establishment.
* **Traffic Encryption** – Handling encryption transparently for both plugins and handlers.
* **Routing** – Receiving `api_->send(uri, ...)` calls from handlers, identifying the correct connector based on the URI scheme, and routing the data through it.
* **Emitting `PacketSignal**` – After decryption and packet verification, signals are dispatched to the handlers.

---

## Ownership Architecture

```
Plugin (libtcp.so)           Core (goodnet_core.so)
──────────────────────────   ──────────────────────────────────────
IConnection (socket, buffer) ConnectionRecord (state, endpoint)
     │                                │
     │  api_->on_connect(ep)          │  generates conn_id
     └──────────────────────────────► │ ◄── conn_id returned to plugin
                                      │
     │  api_->on_data(conn_id, bytes) │  decrypts, assembles packet
     └──────────────────────────────► │ ──► PacketSignal ──► Handlers
                                      │
     ◄────────────────────────────────│  connector_ops_t::send_to(conn_id, bytes)
  do_send(encrypted bytes)            │

```

**Key Principle:** The core never stores pointers to plugin objects, and plugins never receive pointers to core objects. All communication is handled via `conn_id` (uint64_t) and `host_api_t`.

---

## NodeIdentity — Node Identification

Each GoodNet node possesses two Ed25519 keypairs:

| Key | Purpose | Portability |
| --- | --- | --- |
| `user_key` | User Account. This is your "face" on the network—other nodes find you via your `user_pubkey`. | Portable: Copy `~/.goodnet/user_key` to another device. |
| `device_key` | Specific machine identifier. Generated once per device. | Non-portable: Locked to the specific device. |

During registration, a node signs the message `user_pubkey || device_pubkey` with its `user_seckey`. This essentially states: *"I am this user, and I trust this device."*

### Key Storage

```
~/.goodnet/
    user_key    — 64-byte Ed25519 secret key (chmod 600)
    device_key  — 64-byte Ed25519 secret key (chmod 600)

```

At startup: If the files exist, they are loaded; otherwise, new keys are generated.

---

## AUTH Flow

Immediately after a TCP connection is established, both nodes exchange AUTH packets:

```
A ──────────────────────────────────────────────────► B
  header_t { magic=GNET_MAGIC, type=MSG_TYPE_AUTH }
  auth_payload_t {
      user_pubkey[32]    ← Account public key
      device_pubkey[32]  ← Device public key
      signature[64]      ← Ed25519(user_seckey,
                                   user_pubkey || device_pubkey)
  }

B verifies:
  1. GNET_MAGIC and proto_ver match → otherwise, disconnect.
  2. verify(signature, user_pubkey) == ok → otherwise, disconnect.
  3. Saves peer_user_pubkey, peer_device_pubkey.
  4. Adds to pk_index → Node A is now reachable via "gn://user_pubkey_hex".
  5. STATE → ESTABLISHED.

```

---

## Addressing

`ConnectionManager` supports several URI formats for `send()`:

| Format | Example | Description |
| --- | --- | --- |
| `scheme://ip:port` | `tcp://192.168.1.1:8080` | Direct address; connector selected by scheme. |
| `gn://pubkey_hex` | `gn://ab12cd...` | Via account public key (32-byte hex). |
| `alias://name` | `alias://my-node` | Via alias (lookup table in ConnectionManager, TODO). |
| Custom | `myproto://...` | Any scheme—looks for a plugin with `get_scheme() == "myproto"`. |

A connector plugin declares its scheme via `get_scheme()` and remains agnostic of other formats.

---

## Encryption

Encryption is transparent for plugins and handlers:

* **Plugin** passes raw bytes to `api_->on_data()` without decryption.
* **ConnectionManager** decrypts using a session key (X25519 + XSalsa20-Poly1305).
* **Handler** receives clean plaintext via `PacketSignal`.

Private keys never leave the `ConnectionManager`. A plugin can request the core to sign a buffer via `api_->sign_with_device()`, but the key remains secured within the core.

```
Plugin              ConnectionManager           Handler
─────────────────   ─────────────────────────   ────────────
on_data(raw_bytes)
     │
     ▼
  [Encrypted]    ──►  decrypt(session_key)
                       verify(header.signature)
                       emit(PacketSignal)   ──────────────► handle_message(plaintext)

```

### Current Status

| Component | Status |
| --- | --- |
| Ed25519 AUTH (Key signing) | ✅ Implemented |
| TCP Fragment Reassembly | ✅ Implemented |
| X25519 Key Exchange (Session key) | 🔧 TODO |
| XSalsa20-Poly1305 Encryption | 🔧 TODO (Current stub: plaintext) |
| Alias Table | 🔧 TODO |

---

## Plugin API

Plugins communicate with the core **exclusively** through `host_api_t`:

```c
typedef struct {
    // Connector -> Core callbacks (must be called on events)
    conn_id_t (*on_connect)   (void* ctx, const endpoint_t* ep);
    void      (*on_data)      (void* ctx, conn_id_t id, const void* raw, size_t n);
    void      (*on_disconnect)(void* ctx, conn_id_t id, int error_code);

    // Handler requests Core to send data
    void (*send)(void* ctx, const char* uri, uint32_t type,
                 const void* payload, size_t size);

    // Crypto: Plugin requests, Core performs
    int (*sign_with_device) (void* ctx, const void* data, size_t n, uint8_t sig[64]);
    int (*verify_signature) (void* ctx, const void* data, size_t n,
                             const uint8_t* pubkey, const uint8_t* sig);

    void* ctx;  // Opaque context — must be passed as the first argument
} host_api_t;

```

---

## Data Flow

### Incoming Message

```
TCP socket
    │ async_read
    ▼
TcpConnection::do_read()
    │ notify_data(raw, n)
    ▼
api_->on_data(ctx, conn_id, raw, n)
    │
    ▼
ConnectionManager::handle_data()
    │ append to recv_buf
    │ check header_t.magic
    │ assemble complete packet (TCP stream reassembly)
    │
    ├─ MSG_TYPE_AUTH     → process_auth() → STATE_ESTABLISHED
    ├─ MSG_TYPE_KEY_EXCHANGE → TODO
    │
    └─ STATE_ESTABLISHED → decrypt() → PacketSignal::emit()
                                           │
                                           ▼
                                     handler_t::handle_message()

```

### Outgoing Message

```
IHandler::send(uri, type, payload)
    │ api_->send(ctx, uri, type, payload, size)
    ▼
ConnectionManager::send()
    │ resolve_uri(uri) → conn_id
    │   ├─ uri_index["host:port"] → conn_id
    │   ├─ pk_index["pubkey_hex"] → conn_id  (gn:// addressing)
    │   └─ No connection → connector_ops_t::connect(uri)
    │
    │ build header_t + payload
    │ TODO: encrypt(session_key)
    ▼
connector_ops_t::send_to(conn_id, wire_bytes)
    │
    ▼
IConnector → connections_[conn_id]::do_send()
    │
    ▼
TCP socket write

```

---

## Adding a New Connector

A connector is registered in the `ConnectionManager` after the plugin is loaded:

```cpp
// main.cpp — after plugin_mgr.load_all_plugins()
auto opt = plugin_mgr.find_connector_by_scheme("myproto");
if (opt) conn_mgr.register_connector("myproto", *opt);

```

Once registered, any call to `api_->send("myproto://...", ...)` will be automatically routed to this connector.