#pragma once
/// @file sdk/connector.h
/// @brief C interface for transport connector plugins.

#include "plugin.h"

#ifdef __cplusplus
#include <sys/uio.h>   // struct iovec (POSIX); connector may ignore if on Windows
extern "C" {
#endif

/// @brief Connector operations vtable.
///
/// Lifecycle:
///   accept/connect → api->on_connect(ep) → store conn_id
///   recv           → api->on_data(id, buf, len)
///   close          → api->on_disconnect(id, err)
///   send           → core calls send_to() or send_gather() if available
typedef struct connector_ops_t {

    /// Begin async outgoing connection. Returns 0 if accepted, -1 on error.
    int (*connect)(void* ctx, const char* uri);

    /// Start listening. Calls api->on_connect() per accept. Returns 0 or -1.
    int (*listen)(void* ctx, const char* host, uint16_t port);

    /// Write raw bytes to conn_id. Returns 0 or -1.
    int (*send_to)(void* ctx, conn_id_t conn_id, const void* data, size_t size);

    /// Vectored write (gather IO). NULL if not supported — core falls back to send_to.
    /// Returns total bytes written, or -1 on error.
    int (*send_gather)(void* ctx, conn_id_t conn_id,
                       const struct iovec* iov, int iovcnt);

    /// Begin graceful close of conn_id. Must eventually call api->on_disconnect().
    void (*close)(void* ctx, conn_id_t conn_id);

    /// Hard close without drain.
    void (*close_now)(void* ctx, conn_id_t conn_id);

    /// Fill buf with URI scheme: "tcp", "udp", "ws", "ice", "mock", …
    void (*get_scheme)(void* ctx, char* buf, size_t buf_size);

    /// Fill buf with human-readable name.
    void (*get_name)(void* ctx, char* buf, size_t buf_size);

    /// Invoked before dlclose(). Close all connections, stop threads.
    void (*shutdown)(void* ctx);

    /// Opaque plugin context.
    void* connector_ctx;

} connector_ops_t;

typedef int                  (*connector_init_t)   (host_api_t*, connector_ops_t**);
typedef const plugin_info_t* (*plugin_get_info_t)  (void);

#ifdef __cplusplus
}
#endif