#pragma once
#include "../connector.h"
#include <string>
#include <cstring>
#include <span>

namespace gn {

namespace sdk {
/// @brief Abstract connection object for use inside connector implementations.
class IConnection {
public:
    virtual ~IConnection() = default;
    virtual void on_connect   (const endpoint_t* remote)         = 0;
    virtual void on_data      (std::span<const uint8_t> data)    = 0;
    virtual void on_disconnect(int error_code)                    = 0;
    virtual void send         (std::span<const uint8_t> data)    = 0;
    virtual std::string scheme() const                           = 0;
};
} // namespace sdk

/// @defgroup iconnector IConnector C++ Base Class
/// @brief Convenient C++ base for transport plugins.
///
/// Inherit from IConnector, implement virtual methods, add CONNECTOR_PLUGIN(MyClass)
/// at the end of the .cpp file.
///
/// ## Event reporting to the core
/// - Call notify_connect()    after a socket is accepted or connected.
/// - Call notify_data()       whenever bytes arrive from the peer.
/// - Call notify_disconnect() when the socket closes.
///
/// The conn_id returned by notify_connect() must be stored alongside the socket
/// and passed to notify_data() / notify_disconnect().
/// @{
class IConnector {
protected:
    connector_ops_t ops_{};
    host_api_t*     api_ = nullptr;

public:
    IConnector() { ops_.connector_ctx = this; }
    virtual ~IConnector() = default;

    /// @brief Called by CONNECTOR_PLUGIN macro. Do not override.
    void init(host_api_t* api) { api_ = api; on_init(); }

    /// @brief Populate ops_ function table and return the pointer.
    connector_ops_t* to_c_ops() {
        ops_.connect = [](void* ctx, const char* uri) -> int {
            return static_cast<IConnector*>(ctx)->do_connect(uri);
        };
        ops_.listen = [](void* ctx, const char* host, uint16_t port) -> int {
            return static_cast<IConnector*>(ctx)->do_listen(host, port);
        };
        ops_.send_to = [](void* ctx, conn_id_t id,
                          const void* data, size_t size) -> int {
            return static_cast<IConnector*>(ctx)->do_send_to(id, data, size);
        };
        ops_.close = [](void* ctx, conn_id_t id) {
            static_cast<IConnector*>(ctx)->do_close(id);
        };
        ops_.get_scheme = [](void* ctx, char* buf, size_t sz) {
            auto s = static_cast<IConnector*>(ctx)->get_scheme();
            std::strncpy(buf, s.c_str(), sz - 1); buf[sz - 1] = '\0';
        };
        ops_.get_name = [](void* ctx, char* buf, size_t sz) {
            auto n = static_cast<IConnector*>(ctx)->get_name();
            std::strncpy(buf, n.c_str(), sz - 1); buf[sz - 1] = '\0';
        };
        ops_.shutdown = [](void* ctx) {
            static_cast<IConnector*>(ctx)->on_shutdown();
        };
        return &ops_;
    }

    // ── Interface for derived classes ──────────────────────────────────────

    /// @brief Called once on load. Set up io_context, acceptor, thread pool here.
    virtual void on_init()     {}

    /// @brief Called before dlclose(). Close all connections, stop io threads.
    virtual void on_shutdown() {}

    /// @brief Return URI scheme: "tcp", "udp", "ws", "mock", …
    virtual std::string get_scheme() const = 0;

    /// @brief Return human-readable name, e.g. "TCP Connector".
    virtual std::string get_name()   const = 0;

    /// @brief Start async connection to uri. Report result via notify_connect().
    /// @return 0 if request was accepted, -1 on immediate error
    virtual int  do_connect(const char* uri)                       = 0;

    /// @brief Start listening on host:port. Report each accept via notify_connect().
    /// @return 0 on success, -1 on error
    virtual int  do_listen(const char* host, uint16_t port)        = 0;

    /// @brief Write bytes to the connection identified by conn_id.
    /// @return 0 on success, -1 on error
    virtual int  do_send_to(conn_id_t id,
                             const void* data, size_t size)         = 0;

    /// @brief Close the connection. Must eventually call notify_disconnect().
    virtual void do_close(conn_id_t id)                             = 0;

protected:
    /// @brief Register a new connection with the core and obtain conn_id.
    /// @return Assigned conn_id, or CONN_ID_INVALID on failure
    conn_id_t notify_connect(const endpoint_t* ep) {
        if (api_ && api_->on_connect) return api_->on_connect(api_->ctx, ep);
        return CONN_ID_INVALID;
    }

    /// @brief Forward raw bytes to the core for reassembly and dispatch.
    void notify_data(conn_id_t id, const void* data, size_t size) {
        if (api_ && api_->on_data) api_->on_data(api_->ctx, id, data, size);
    }

    /// @brief Notify the core that a connection has closed.
    /// @param error 0 = clean close, non-zero = error code
    void notify_disconnect(conn_id_t id, int error = 0) {
        if (api_ && api_->on_disconnect) api_->on_disconnect(api_->ctx, id, error);
    }
};

/// @}  // defgroup iconnector

} // namespace gn
