#pragma once
/// @file sdk/plugin.h
/// @brief Host API vtable — the sole communication channel between a plugin and the core.
///
/// Every plugin (handler or connector) receives a `host_api_t*` pointer during
/// its init call.  This vtable exposes all core services to plugins:
///   - Network I/O: send, broadcast, disconnect
///   - Cryptographic operations: sign, verify
///   - Routing: find connections by pubkey
///   - Configuration: read config values
///   - Logging: portable log shim sharing the core's spdlog instance
///
/// Thread-safety: all function pointers are safe to call from any thread
/// after the plugin's `*_init()` returns.  The `ctx` field is always passed
/// as the first argument — do NOT cache or modify it.
///
/// Ownership: the host_api_t and its plugin_info are owned by PluginManager.
/// They remain valid for the entire plugin lifetime (until shutdown() returns).

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct handler_t       handler_t;
typedef struct connector_ops_t connector_ops_t;

/// @brief Host API vtable injected into every plugin before its *_init() call.
///
/// Every function receives `ctx` as its first argument — this is an opaque
/// pointer to the core's internal state.  Always use `api->ctx` for this.
typedef struct host_api_t {

    // ── Connector -> Core ─────────────────────────────────────────────────────

    /// @brief Register a new inbound connection.
    /// @param ctx  Core context (api->ctx).
    /// @param ep   Endpoint descriptor filled by the connector.
    ///             The core copies the struct — caller may free after return.
    /// @return Assigned conn_id (unique, monotonic), or CONN_ID_INVALID on error.
    ///
    /// Thread-safety: safe from any connector thread.
    conn_id_t (*on_connect)(void* ctx, const endpoint_t* ep);

    /// @brief Deliver raw bytes from the network.
    /// @param ctx   Core context.
    /// @param id    Connection ID from on_connect().
    /// @param raw   Raw wire bytes (may contain partial frames).
    /// @param size  Byte count.
    ///
    /// The core handles frame reassembly, AEAD decryption, and dispatch.
    /// Thread-safety: safe to call concurrently for different conn_ids.
    /// Calling concurrently for the SAME conn_id is undefined behavior.
    void (*on_data)(void* ctx, conn_id_t id, const void* raw, size_t size);

    /// @brief Notify that a connection closed (from connector side).
    /// @param ctx         Core context.
    /// @param id          Connection ID.
    /// @param error_code  0 = clean close, non-zero = error (errno-style).
    void (*on_disconnect)(void* ctx, conn_id_t id, int error_code);

    // ── Handler -> Core ───────────────────────────────────────────────────────

    /// @brief Send a packet to a peer by URI.
    /// @param ctx       Core context.
    /// @param uri       Peer address ("tcp://host:port" or hex pubkey).
    /// @param msg_type  Wire message type (MSG_TYPE_*).
    /// @param payload   Raw payload bytes (copied by the core).
    /// @param size      Payload byte count.
    ///
    /// The core resolves the route, encrypts with AEAD, and transmits.
    /// If no connection exists, auto-connect is triggered and the message
    /// is queued until the handshake completes.
    void (*send)(void* ctx, const char* uri, uint32_t msg_type,
                 const void* payload, size_t size);

    /// @brief Send a response directly on a connection (by conn_id).
    /// @param ctx      Core context.
    /// @param conn_id  Connection ID (use endpoint_t::peer_id from handle_message).
    /// @param msg_type Wire message type.
    /// @param data     Payload bytes (copied).
    /// @param len      Payload byte count.
    ///
    /// Skips URI resolution — use when you already know the conn_id.
    void (*send_response)(void* ctx, conn_id_t conn_id, uint32_t msg_type,
                          const void* data, size_t len);

    /// @brief Broadcast to all ESTABLISHED peers.
    /// @param ctx       Core context.
    /// @param msg_type  Wire message type.
    /// @param payload   Payload bytes (copied per-peer).
    /// @param size      Payload byte count.
    void (*broadcast)(void* ctx, uint32_t msg_type,
                      const void* payload, size_t size);

    /// @brief Initiate graceful close of a connection.
    /// @param ctx      Core context.
    /// @param conn_id  Connection to close.
    ///
    /// The send queue is drained before the connection closes.
    /// The connector's on_disconnect() callback will fire asynchronously.
    void (*disconnect)(void* ctx, conn_id_t conn_id);

    // ── Crypto ────────────────────────────────────────────────────────────────

    /// @brief Sign data with the node's device Ed25519 key.
    /// @param ctx   Core context.
    /// @param data  Data to sign.
    /// @param size  Data byte count.
    /// @param sig   Output: GN_SIGN_BYTES (64) bytes of Ed25519 signature.
    /// @return 0 on success, -1 on error.
    int (*sign_with_device)(void* ctx, const void* data, size_t size,
                             uint8_t sig[GN_SIGN_BYTES]);

    /// @brief Verify an Ed25519 signature.
    /// @param ctx        Core context.
    /// @param data       Signed data.
    /// @param size       Data byte count.
    /// @param pubkey     Ed25519 public key (GN_SIGN_PUBLICKEYBYTES bytes).
    /// @param signature  Signature to verify (GN_SIGN_BYTES bytes).
    /// @return 0 if valid, non-zero if invalid.
    int (*verify_signature)(void* ctx, const void* data, size_t size,
                             const uint8_t* pubkey, const uint8_t* signature);

    // ── Routing ───────────────────────────────────────────────────────────────

    /// @brief Look up conn_id for an ESTABLISHED peer by hex-encoded pubkey.
    /// @param ctx            Core context.
    /// @param pubkey_hex_64  Hex-encoded Ed25519 user pubkey (64 chars, NUL-terminated).
    /// @return conn_id, or CONN_ID_INVALID if not found or not ESTABLISHED.
    conn_id_t (*find_conn_by_pubkey)(void* ctx, const char* pubkey_hex_64);

    /// @brief Fill endpoint descriptor for a connection.
    /// @param ctx      Core context.
    /// @param conn_id  Connection ID.
    /// @param ep       Output: filled with peer's address, port, pubkey, flags.
    /// @return 0 on success, -1 if conn_id not found.
    int (*get_peer_info)(void* ctx, conn_id_t conn_id, endpoint_t* ep);

    // ── Config ────────────────────────────────────────────────────────────────

    /// @brief Read a config value as a NUL-terminated string.
    /// @param ctx       Core context.
    /// @param key       Dotted config key (e.g. "core.listen_port", "plugins.base_dir").
    /// @param buf       Output buffer.
    /// @param buf_size  Buffer capacity (including NUL).
    /// @return Bytes written (excluding NUL), or -1 if key not recognized.
    int (*config_get)(void* ctx, const char* key, char* buf, size_t buf_size);

    // ── Self-registration ─────────────────────────────────────────────────────

    /// @brief Register an additional handler from within a connector plugin.
    /// @param ctx  Core context.
    /// @param h    Handler descriptor (plugin-owned, must outlive the registration).
    ///
    /// @warning Only safe during on_init().  Calling after the first packet
    ///          arrives is undefined behavior.
    void (*register_handler)(void* ctx, handler_t* h);

    /// @brief Register a secondary transport path for an already-ESTABLISHED peer.
    /// @param ctx          Core context.
    /// @param pubkey_hex   Hex-encoded Ed25519 user pubkey of the peer (64 chars).
    /// @param ep           Endpoint descriptor for the new transport.
    /// @param scheme       Transport scheme ("ice", "ws", ...).
    /// @return transport_conn_id for the new path, or CONN_ID_INVALID on error.
    ///
    /// The core adds a TransportPath to the existing ConnectionRecord.
    /// Data arriving on transport_conn_id is processed through the primary session.
    /// The connector calls on_data(transport_conn_id, ...) for inbound data,
    /// and the core calls send_to(transport_conn_id, ...) for outbound.
    conn_id_t (*add_transport)(void* ctx, const char* pubkey_hex,
                                const endpoint_t* ep, const char* scheme);

    // ── Logging ───────────────────────────────────────────────────────────────

    /// @brief Portable log shim that writes through the core's spdlog instance.
    /// @param ctx    Core context.
    /// @param level  0=trace, 1=debug, 2=info, 3=warn, 4=error, 5=critical.
    /// @param file   Source file name (__FILE__).
    /// @param line   Source line number (__LINE__).
    /// @param msg    NUL-terminated log message.
    void (*log)(void* ctx, int level, const char* file, int line, const char* msg);

    // ── Metadata ──────────────────────────────────────────────────────────────

    /// @brief Raw spdlog::logger* for logger sharing across .so boundaries.
    ///        Allows plugins linked against the same spdlog to log natively.
    ///        May be NULL if the core was built without spdlog.
    void*          internal_logger;

    /// @brief Plugin metadata, owned by PluginManager.
    ///        Valid for the entire plugin lifetime.  Do not free.
    plugin_info_t* plugin_info;

    /// @brief Opaque core context — pass as first argument to every callback.
    void* ctx;

} host_api_t;

/// @brief Optional metadata export — called before `*_init()`.
///
/// If a plugin exports this symbol, PluginManager reads the metadata
/// without initializing the plugin.  Useful for dependency graphs and
/// capability scanning.
///
/// @code
/// extern "C" GN_EXPORT
/// const plugin_info_t* plugin_get_info() {
///     static plugin_info_t info{"my_plugin", 0x00010000, 128, {}, 0};
///     return &info;
/// }
/// @endcode
typedef const plugin_info_t* (*plugin_get_info_t)(void);

#ifdef __cplusplus
}
#endif
