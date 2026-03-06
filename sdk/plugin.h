#pragma once
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @defgroup host_api Host API
/// @brief Sole communication channel between a plugin and the core.
///
/// Filled by ConnectionManager::fill_host_api() and injected into every
/// plugin at init time. Plugins must never depend on core headers directly.
///
/// Rule: every callback receives `ctx` as its first argument, enabling
/// multiple core instances within a single process.
/// @{

typedef struct host_api_t {

    /// @name Connector → Core callbacks
    /// The plugin **must** call these when transport events occur.
    /// @{

    /// @brief Register a new connection. Returns conn_id to store next to the socket.
    /// @param ctx      Opaque core context (pass as-is)
    /// @param endpoint Remote peer address
    /// @return Assigned conn_id, or CONN_ID_INVALID on failure
    conn_id_t (*on_connect)(void* ctx, const endpoint_t* endpoint);

    /// @brief Deliver raw bytes from the network to the core.
    ///        The core performs TCP reassembly internally.
    /// @param ctx   Opaque core context
    /// @param id    conn_id returned by on_connect()
    /// @param raw   Raw bytes received from the socket
    /// @param size  Byte count
    void (*on_data)(void* ctx, conn_id_t id, const void* raw, size_t size);

    /// @brief Notify the core that a connection has closed.
    /// @param ctx        Opaque core context
    /// @param id         conn_id of the closed connection
    /// @param error_code 0 = clean close, non-zero = error
    void (*on_disconnect)(void* ctx, conn_id_t id, int error_code);

    /// @}

    /// @name Handler → Core callbacks
    /// @{

    /// @brief Send a packet to a peer identified by URI.
    ///        ConnectionManager resolves the route and selects the connector.
    /// @param ctx      Opaque core context
    /// @param uri      Destination: "scheme://host:port" or "host:port"
    /// @param msg_type MSG_TYPE_* value
    /// @param payload  Payload bytes (will be encrypted for remote peers)
    /// @param size     Payload byte count
    void (*send)(void*       ctx,
                 const char* uri,
                 uint32_t    msg_type,
                 const void* payload,
                 size_t      size);

    /// @}

    /// @name Crypto helpers
    /// The device private key never leaves the core.
    /// @{

    /// @brief Sign data with the device Ed25519 secret key.
    /// @param ctx  Opaque core context
    /// @param data Buffer to sign
    /// @param size Buffer byte count
    /// @param sig  Output: 64-byte Ed25519 signature
    /// @return 0 on success
    int (*sign_with_device)(void*       ctx,
                            const void* data,
                            size_t      size,
                            uint8_t     sig[64]);

    /// @brief Verify an Ed25519 signature.
    /// @return 0 if signature is valid
    int (*verify_signature)(void*          ctx,
                            const void*    data,
                            size_t         size,
                            const uint8_t* pubkey,
                            const uint8_t* signature);

    /// @}

    /// @brief Raw pointer to the spdlog::logger instance.
    ///        Used by sync_plugin_context() to inject the logger into plugins
    ///        without requiring RTLD_GLOBAL.
    void* internal_logger;

    /// @brief Plugin role set by PluginManager before *_init() is called.
    plugin_type_t plugin_type;

    /// @brief Opaque core context. Must be passed as the first argument
    ///        to every callback above.
    void* ctx;

} host_api_t;

/// @}  // defgroup host_api

#ifdef __cplusplus
}
#endif
