#pragma once
#include "plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @defgroup connector Connector Plugin C API
/// @brief C interface for transport plugins (TCP, UDP, WebSocket, …).
///
/// ## Ownership model
/// - The **plugin** owns connection objects (sockets, state machines, …).
/// - The **core** (ConnectionManager) owns ConnectionRecord, keyed by conn_id_t.
/// - conn_id_t is obtained from api->on_connect() and must be stored alongside
///   the socket for use in subsequent on_data() and on_disconnect() calls.
///
/// ## Connection lifecycle
/// 1. **Incoming**: accept → api->on_connect(ep) → store conn_id next to socket
/// 2. **Outgoing**: async_connect → api->on_connect(ep) → store conn_id
/// 3. **Data**:     read bytes → api->on_data(conn_id, buf, len)
/// 4. **Close**:    socket closed → api->on_disconnect(conn_id, error)
/// 5. **Send**:     core calls send_to(conn_id, data, size)
/// @{
typedef struct connector_ops_t {

    /// @brief Initiate an outgoing connection to uri (e.g., "tcp://host:4242").
    ///        Must return quickly; actual connection happens asynchronously.
    ///        On success: call api->on_connect() to register with the core.
    /// @return 0 if request accepted, -1 on immediate error
    int (*connect)(void* connector_ctx, const char* uri);

    /// @brief Start listening for incoming connections on host:port.
    ///        For each accepted socket: call api->on_connect().
    /// @return 0 on success, -1 on error
    int (*listen)(void* connector_ctx, const char* host, uint16_t port);

    /// @brief Send raw bytes to the connection identified by conn_id.
    /// @return 0 on success, -1 on error
    int (*send_to)(void*       connector_ctx,
                   conn_id_t   conn_id,
                   const void* data,
                   size_t      size);

    /// @brief Close connection conn_id.
    ///        The plugin must eventually call api->on_disconnect(conn_id, error).
    void (*close)(void* connector_ctx, conn_id_t conn_id);

    /// @brief Fill buf with the URI scheme: "tcp", "udp", "ws", "mock", …
    ///        Used by PluginManager::find_connector_by_scheme().
    void (*get_scheme)(void* connector_ctx, char* buf, size_t buf_size);

    /// @brief Fill buf with a human-readable name: "TCP Connector", …
    void (*get_name)(void* connector_ctx, char* buf, size_t buf_size);

    /// @brief Invoked by the core before dlclose(). Close all connections,
    ///        stop io_context, free all resources.
    void (*shutdown)(void* connector_ctx);

    /// @brief Opaque plugin context, typically `this`.
    void* connector_ctx;

} connector_ops_t;

/// @brief Connector plugin entry point signature.
/// @param api     Host API provided by the core
/// @param out_ops Output: pointer to connector operations table
/// @return 0 on success, non-zero on failure
typedef int (*connector_init_t)(host_api_t* api, connector_ops_t** out_ops);

/// @}  // defgroup connector

#ifdef __cplusplus
}
#endif
