#pragma once
/// @file sdk/plugin.h
/// @brief Host API — sole communication channel between a plugin and the core.

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations (defined in handler.h / connector.h)
typedef struct handler_t       handler_t;
typedef struct connector_ops_t connector_ops_t;

/// @brief Host API vtable injected into every plugin before its *_init() call.
///        Every callback receives ctx as its first argument, allowing multiple
///        core instances in the same process.
typedef struct host_api_t {

    // ── Connector → Core ──────────────────────────────────────────────────────

    /// Register a new connection; returns the conn_id to associate with the socket.
    conn_id_t (*on_connect)(void* ctx, const endpoint_t* endpoint);

    /// Deliver raw bytes received from the network (core handles reassembly).
    void (*on_data)(void* ctx, conn_id_t id, const void* raw, size_t size);

    /// Notify the core that a connection has closed.
    void (*on_disconnect)(void* ctx, conn_id_t id, int error_code);

    // ── Handler → Core ────────────────────────────────────────────────────────

    /// Send a packet to a peer by URI.  Core resolves route and encrypts.
    void (*send)(void* ctx, const char* uri, uint32_t msg_type,
                 const void* payload, size_t size);

    /// Send a response directly on an existing connection (no URI lookup).
    /// Use endpoint->peer_id (== conn_id) from inside handle_message().
    void (*send_response)(void* ctx, conn_id_t conn_id, uint32_t msg_type,
                          const void* data, size_t len);

    // ── Crypto ────────────────────────────────────────────────────────────────

    /// Sign data with the node's device Ed25519 secret key.  Returns 0 on success.
    int (*sign_with_device)(void* ctx, const void* data, size_t size,
                             uint8_t sig[64]);

    /// Verify an Ed25519 signature.  Returns 0 if valid.
    int (*verify_signature)(void* ctx, const void* data, size_t size,
                             const uint8_t* pubkey, const uint8_t* signature);

    // ── Routing helpers ───────────────────────────────────────────────────────

    /// Look up the conn_id for a currently-ESTABLISHED peer by hex-encoded pubkey.
    /// Returns CONN_ID_INVALID if not connected or not yet established.
    conn_id_t (*find_conn_by_pubkey)(void* ctx, const char* pubkey_hex_64);

    // ── Self-registration ─────────────────────────────────────────────────────

    /// Register an additional handler_t from within a connector plugin.
    /// Use during on_init() only — unsafe after the first packet arrives.
    void (*register_handler)(void* ctx, handler_t* h);

    // ── Logging ───────────────────────────────────────────────────────────────

    /// Portable log shim for plugins that cannot link spdlog directly.
    /// level: 0=trace  1=debug  2=info  3=warn  4=error  5=critical
    void (*log)(void* ctx, int level, const char* file, int line,
                const char* msg);

    // ── Metadata ──────────────────────────────────────────────────────────────

    /// Raw spdlog::logger*. Passed to sync_plugin_context() for logger sharing
    /// without RTLD_GLOBAL.
    void* internal_logger;

    /// Filled by PluginManager before *_init().  Plugin reads its own info here.
    /// Points to a plugin_info_t owned by PluginManager (not by the plugin).
    plugin_info_t* plugin_info;

    /// @deprecated Zero-filled; kept so old .so files that read this offset
    ///             don't crash. Use plugin_info->caps_mask in new code.
    plugin_type_t plugin_type;

    /// Opaque core context — pass as first argument to every callback above.
    void* ctx;

} host_api_t;

/// @}  // defgroup host_api

#ifdef __cplusplus
}
#endif