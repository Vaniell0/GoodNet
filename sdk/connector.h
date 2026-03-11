#pragma once
/// @file sdk/connector.h
/// @brief C interface for transport connector plugins (TCP, UDP, WebSocket, ICE…).

#include "plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Connector operations vtable.
///
/// Ownership model
/// ───────────────
/// The plugin owns connection objects (sockets, state machines).
/// The core (ConnectionManager) owns ConnectionRecord, keyed by conn_id_t.
/// conn_id_t is obtained from api->on_connect() and must be stored alongside
/// the socket for use in subsequent on_data() / on_disconnect() calls.
///
/// Lifecycle
/// ─────────
/// 1. Incoming:  accept   → api->on_connect(ep)  → store conn_id
/// 2. Outgoing:  async_connect → api->on_connect(ep) → store conn_id
/// 3. Data:      read     → api->on_data(conn_id, buf, len)
/// 4. Close:     close    → api->on_disconnect(conn_id, error)
/// 5. Send:      core calls send_to(conn_id, data, size)
typedef struct connector_ops_t {

    /// @brief Start async outgoing connection to uri ("tcp://host:port").
    ///        Must return quickly — actual connect happens asynchronously.
    /// @return 0 if request accepted, -1 on immediate error
    int (*connect)(void* connector_ctx, const char* uri);

    /// @brief Start listening on host:port.  Call api->on_connect() per accept.
    /// @return 0 on success, -1 on error
    int (*listen)(void* connector_ctx, const char* host, uint16_t port);

    /// @brief Write raw bytes to conn_id.
    /// @return 0 on success, -1 on error
    int (*send_to)(void* connector_ctx, conn_id_t conn_id,
                   const void* data, size_t size);

    /// @brief Close conn_id.  Must eventually call api->on_disconnect().
    void (*close)(void* connector_ctx, conn_id_t conn_id);

    /// @brief Fill buf with URI scheme: "tcp", "udp", "ws", "ice", "mock", …
    void (*get_scheme)(void* connector_ctx, char* buf, size_t buf_size);

    /// @brief Fill buf with human-readable name: "GoodNet TCP Connector", …
    void (*get_name)(void* connector_ctx, char* buf, size_t buf_size);

    /// @brief Invoked before dlclose().  Close all connections, free resources.
    void (*shutdown)(void* connector_ctx);

    /// Opaque plugin context, typically `this`.
    void* connector_ctx;

} connector_ops_t;

/// @brief Connector plugin entry point.
typedef int (*connector_init_t)(host_api_t* api, connector_ops_t** out_ops);

/// @brief Optional metadata export (same as handler side).
typedef const plugin_info_t* (*plugin_get_info_t)(void);

#ifdef __cplusplus
}
#endif
