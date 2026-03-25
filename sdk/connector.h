#pragma once
/// @file sdk/connector.h
/// @brief C interface for transport connector plugins.
///
/// A connector plugin provides a transport layer (TCP, UDP, ICE, WebSocket, etc.)
/// to the GoodNet core.  The core uses the connector's vtable for all network I/O.
///
/// ## Plugin lifecycle
///   1. Core calls `connector_init(api, &ops)` with the host API.
///   2. Plugin fills a `connector_ops_t` struct and returns 0.
///   3. Core calls `listen()` to start accepting connections.
///   4. Core calls `connect()` for outbound connections.
///   5. Core calls `send_to()` / `send_gather()` to transmit frames.
///   6. On shutdown, core calls `ops->shutdown(ctx)`.
///
/// ## Connector -> Core notifications
///   - New inbound connection: call `api->on_connect(ep)` -> get conn_id.
///   - Received data: call `api->on_data(id, buf, len)`.
///   - Connection closed: call `api->on_disconnect(id, err)`.
///
/// ## Thread-safety
///   - `send_to()` and `send_gather()` may be called from any core thread.
///   - `on_connect()`, `on_data()`, `on_disconnect()` are safe to call from
///     any connector thread.
///   - `connect()` and `listen()` are called from the core's IO thread.

#include "plugin.h"

#ifdef __cplusplus
#if defined(_WIN32)
struct iovec { void* iov_base; size_t iov_len; };
#else
#include <sys/uio.h>
#endif
extern "C" {
#endif

/// @brief Connector operations vtable.
///
/// All function pointers receive `connector_ctx` as their first argument.
/// The plugin sets `connector_ctx` to its own state pointer (typically `this`
/// for C++ plugins).
typedef struct connector_ops_t {

    /// @brief Initiate an async outgoing connection.
    /// @param ctx  Connector context (connector_ops_t::connector_ctx).
    /// @param uri  Target address ("host:port" or scheme-specific URI).
    /// @return 0 if connection attempt started, -1 on immediate error.
    ///
    /// On success, the connector must eventually call `api->on_connect(ep)`
    /// when the connection is established, followed by `api->on_data()` for
    /// received bytes.
    int (*connect)(void* ctx, const char* uri);

    /// @brief Start listening for inbound connections.
    /// @param ctx   Connector context.
    /// @param host  Bind address (e.g. "0.0.0.0").
    /// @param port  Bind port.
    /// @return 0 on success, -1 on error (e.g. port in use).
    ///
    /// For each accepted connection, the connector calls `api->on_connect(ep)`.
    int (*listen)(void* ctx, const char* host, uint16_t port);

    /// @brief Write raw bytes to a connection.
    /// @param ctx   Connector context.
    /// @param conn_id  Target connection.
    /// @param data  Bytes to send (core-owned buffer, valid for the call duration).
    /// @param size  Byte count.
    /// @return 0 on success, -1 on error.
    int (*send_to)(void* ctx, conn_id_t conn_id, const void* data, size_t size);

    /// @brief Vectored write (gather IO).
    /// @param ctx      Connector context.
    /// @param conn_id  Target connection.
    /// @param iov      IO vector array.
    /// @param iovcnt   Number of iov entries.
    /// @return Total bytes written, or -1 on error.
    ///
    /// If NULL, the core falls back to calling `send_to()` per segment.
    /// Implementing this is optional but improves performance by reducing
    /// system call overhead for multi-part frames.
    int (*send_gather)(void* ctx, conn_id_t conn_id,
                       const struct iovec* iov, int iovcnt);

    /// @brief Begin graceful close of a connection.
    /// @param ctx      Connector context.
    /// @param conn_id  Connection to close.
    ///
    /// The connector should drain any pending writes, then call
    /// `api->on_disconnect(conn_id, 0)` when fully closed.
    void (*close)(void* ctx, conn_id_t conn_id);

    /// @brief Hard close without draining pending writes.
    /// @param ctx      Connector context.
    /// @param conn_id  Connection to abort.
    ///
    /// Used during shutdown or error recovery.  The connector must still
    /// call `api->on_disconnect()` afterward.
    void (*close_now)(void* ctx, conn_id_t conn_id);

    /// @brief Get the URI scheme this connector handles.
    /// @param ctx       Connector context.
    /// @param buf       Output buffer.
    /// @param buf_size  Buffer capacity.
    ///
    /// Examples: "tcp", "udp", "ws", "ice".
    /// The core uses this for URI-based routing (e.g. "tcp://host:port").
    void (*get_scheme)(void* ctx, char* buf, size_t buf_size);

    /// @brief Get a human-readable connector name.
    /// @param ctx       Connector context.
    /// @param buf       Output buffer.
    /// @param buf_size  Buffer capacity.
    ///
    /// Used in log messages and diagnostics.
    void (*get_name)(void* ctx, char* buf, size_t buf_size);

    /// @brief Shutdown the connector — close all connections, stop threads.
    /// @param ctx  Connector context.
    ///
    /// Called by the core before `dlclose()`.  After this returns, no further
    /// vtable calls will be made and the plugin's address space becomes invalid.
    ///
    /// The connector MUST:
    ///   1. Stop accepting new connections.
    ///   2. Close all existing connections (call on_disconnect for each).
    ///   3. Join/stop all internal threads.
    void (*shutdown)(void* ctx);

    /// @brief Opaque connector context.
    ///        Set by the plugin during `connector_init()`.
    ///        Passed as the first argument to every vtable function.
    void* connector_ctx;

} connector_ops_t;

/// @brief Connector plugin entry point.
///
/// Exported as `connector_init` (with `GN_EXPORT`).  The core calls this once
/// during plugin loading.  The plugin MUST:
///   1. Initialize internal state using `api` for core interactions.
///   2. Fill `*out_ops` with a pointer to a valid `connector_ops_t`.
///   3. Return 0 on success, non-zero on failure.
///
/// @param api      Host API vtable (valid for plugin lifetime).
/// @param out_ops  Output: plugin-owned ops vtable.
/// @return 0 on success, non-zero on failure.
typedef int (*connector_init_t)(host_api_t*, connector_ops_t**);

#ifdef __cplusplus
}
#endif
