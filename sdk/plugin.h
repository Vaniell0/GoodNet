#pragma once
/// @file sdk/plugin.h
/// @brief Host API vtable — sole communication channel between a plugin and the core.

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct handler_t       handler_t;
typedef struct connector_ops_t connector_ops_t;

/// Host API vtable injected into every plugin before its *_init() call.
/// Every function receives @p ctx as its first argument.
typedef struct host_api_t {

    // ── Connector → Core ──────────────────────────────────────────────────────

    /// Register a new inbound connection; returns the conn_id.
    conn_id_t (*on_connect)(void* ctx, const endpoint_t* ep);

    /// Deliver raw bytes from the network; core handles reassembly.
    void (*on_data)(void* ctx, conn_id_t id, const void* raw, size_t size);

    /// Notify that a connection closed.
    void (*on_disconnect)(void* ctx, conn_id_t id, int error_code);

    // ── Handler → Core ────────────────────────────────────────────────────────

    /// Send a packet to a peer by URI; core resolves route and encrypts.
    void (*send)(void* ctx, const char* uri, uint32_t msg_type,
                 const void* payload, size_t size);

    /// Send a response directly on conn_id (use ep->peer_id from handle_message).
    void (*send_response)(void* ctx, conn_id_t conn_id, uint32_t msg_type,
                          const void* data, size_t len);

    /// Broadcast to all ESTABLISHED peers.
    void (*broadcast)(void* ctx, uint32_t msg_type,
                      const void* payload, size_t size);

    /// Initiate graceful close of a connection.
    void (*disconnect)(void* ctx, conn_id_t conn_id);

    // ── Crypto ────────────────────────────────────────────────────────────────

    /// Sign data with the node device Ed25519 key. Returns 0 on success.
    int (*sign_with_device)(void* ctx, const void* data, size_t size,
                             uint8_t sig[64]);

    /// Verify an Ed25519 signature. Returns 0 if valid.
    int (*verify_signature)(void* ctx, const void* data, size_t size,
                             const uint8_t* pubkey, const uint8_t* signature);

    // ── Routing ───────────────────────────────────────────────────────────────

    /// Look up the conn_id for an ESTABLISHED peer by hex-encoded pubkey (64 chars).
    conn_id_t (*find_conn_by_pubkey)(void* ctx, const char* pubkey_hex_64);

    /// Fill @p ep with endpoint details for conn_id. Returns 0 on success.
    int (*get_peer_info)(void* ctx, conn_id_t conn_id, endpoint_t* ep);

    // ── Config ────────────────────────────────────────────────────────────────

    /// Read a config value as a NUL-terminated string into @p buf.
    /// Returns the number of bytes written (excluding NUL), or -1 on error.
    int (*config_get)(void* ctx, const char* key, char* buf, size_t buf_size);

    // ── Self-registration ─────────────────────────────────────────────────────

    /// Register an additional handler_t from within a connector plugin.
    /// Safe only during on_init(); undefined behaviour after first packet.
    void (*register_handler)(void* ctx, handler_t* h);

    // ── Logging ───────────────────────────────────────────────────────────────

    /// Portable log shim. level: 0=trace 1=debug 2=info 3=warn 4=error 5=critical
    void (*log)(void* ctx, int level, const char* file, int line, const char* msg);

    // ── Metadata ──────────────────────────────────────────────────────────────

    void*          internal_logger; ///< Raw spdlog::logger* for logger sharing.
    plugin_info_t* plugin_info;     ///< Owned by PluginManager; valid for plugin lifetime.

    /// @deprecated Zero-filled; kept for old ABI consumers. Use plugin_info->caps_mask.
    plugin_type_t  plugin_type;

    void* ctx; ///< Opaque core context — pass as first arg to every callback.

} host_api_t;

#ifdef __cplusplus
}
#endif
