#pragma once
/// @file sdk/cpp/connector.hpp
/// @brief C++ base class for transport connector plugins.
///
/// Provides a type-safe C++ interface over the C `connector_ops_t` ABI.
/// Subclass `gn::IConnector` and implement the pure virtual methods, then
/// use the `CONNECTOR_PLUGIN(ClassName)` macro to generate the entry point.
///
/// ## Minimal example
/// @code
/// class TcpConnector : public gn::IConnector {
/// public:
///     std::string get_scheme() const override { return "tcp"; }
///     std::string get_name()   const override { return "TCP Transport"; }
///
///     int do_connect(const char* uri)                          override { /* ... */ }
///     int do_listen(const char* host, uint16_t port)           override { /* ... */ }
///     int do_send(conn_id_t id, std::span<const uint8_t> data) override { /* ... */ }
///     void do_close(conn_id_t id, bool hard)                    override { /* ... */ }
/// };
/// CONNECTOR_PLUGIN(TcpConnector)
/// @endcode

#include "../sdk/connector.h"
#include <cstring>
#include <span>
#include <string>

#if defined(_WIN32)
struct iovec { void* iov_base; size_t iov_len; };
#else
#include <sys/uio.h>
#endif

namespace gn {

/// @brief C++ base class for transport connector plugins.
///
/// Wraps the C `connector_ops_t` ABI with type-safe virtual methods and
/// notification helpers for core callbacks.
///
/// Ownership: the connector instance is typically a file-scope static,
/// created by the CONNECTOR_PLUGIN macro.
class IConnector {
protected:
    connector_ops_t ops_{};
    host_api_t*     api_ = nullptr;

public:
    IConnector()  { ops_.connector_ctx = this; }
    virtual ~IConnector() = default;

    IConnector(const IConnector&)            = delete;
    IConnector& operator=(const IConnector&) = delete;

    /// @brief Called by the CONNECTOR_PLUGIN macro.  Stores the host API and
    ///        invokes on_init() for plugin-specific setup.
    void init(host_api_t* api) { api_ = api; on_init(); }

    /// @brief Build the C connector_ops_t from the virtual interface.
    /// @return Pointer to the internal ops (plugin-owned).
    connector_ops_t* to_c_ops() {
        ops_.connect     = [](void* ctx, const char* uri) -> int {
            return static_cast<IConnector*>(ctx)->do_connect(uri); };
        ops_.listen      = [](void* ctx, const char* h, uint16_t p) -> int {
            return static_cast<IConnector*>(ctx)->do_listen(h, p); };
        ops_.send_to     = [](void* ctx, conn_id_t id, const void* d, size_t sz) -> int {
            return static_cast<IConnector*>(ctx)->do_send(id, {static_cast<const uint8_t*>(d), sz}); };
        ops_.send_gather = [](void* ctx, conn_id_t id, const struct iovec* iov, int n) -> int {
            return static_cast<IConnector*>(ctx)->do_send_gather(id, iov, n); };
        ops_.close       = [](void* ctx, conn_id_t id) {
            static_cast<IConnector*>(ctx)->do_close(id, false); };
        ops_.close_now   = [](void* ctx, conn_id_t id) {
            static_cast<IConnector*>(ctx)->do_close(id, true); };
        ops_.get_scheme  = [](void* ctx, char* buf, size_t sz) {
            auto s = static_cast<IConnector*>(ctx)->get_scheme();
            std::strncpy(buf, s.c_str(), sz - 1); buf[sz - 1] = '\0'; };
        ops_.get_name    = [](void* ctx, char* buf, size_t sz) {
            auto n = static_cast<IConnector*>(ctx)->get_name();
            std::strncpy(buf, n.c_str(), sz - 1); buf[sz - 1] = '\0'; };
        ops_.shutdown    = [](void* ctx) {
            static_cast<IConnector*>(ctx)->on_shutdown(); };
        return &ops_;
    }

    // ── Interface (override in subclass) ──────────────────────────────────────

    /// @brief Called after the host API is set.  Start internal threads here.
    virtual void on_init()     {}

    /// @brief Called before dlclose().  Stop all threads, close all connections.
    virtual void on_shutdown() {}

    /// @brief Return the URI scheme (e.g. "tcp", "udp", "ice").
    virtual std::string get_scheme() const = 0;

    /// @brief Return a human-readable connector name for logging.
    virtual std::string get_name()   const = 0;

    /// @brief Initiate an outgoing connection.
    /// @param uri  Target address (scheme-specific).
    /// @return 0 if started, -1 on immediate error.
    virtual int  do_connect(const char* uri)                           = 0;

    /// @brief Start listening on host:port.
    /// @return 0 on success, -1 on error.
    virtual int  do_listen (const char* host, uint16_t port)           = 0;

    /// @brief Send a contiguous byte span to a connection.
    /// @return 0 on success, -1 on error.
    virtual int  do_send(conn_id_t id, std::span<const uint8_t> data)  = 0;

    /// @brief Vectored send (gather IO).
    ///
    /// Default implementation: iterate iov segments and call do_send() each.
    /// Override for zero-copy sendmsg/writev support.
    virtual int  do_send_gather(conn_id_t id, const struct iovec* iov, int n) {
        int rc = 0;
        for (int i = 0; i < n; ++i) {
            rc = do_send(id, {static_cast<const uint8_t*>(iov[i].iov_base),
                               iov[i].iov_len});
            if (rc != 0) break;
        }
        return rc;
    }

    /// @brief Close a connection.
    /// @param id    Connection to close.
    /// @param hard  true = abort immediately, false = graceful drain.
    virtual void do_close(conn_id_t id, bool hard)                     = 0;

    /// @brief Return plugin metadata.  Override for custom version/priority.
    virtual const plugin_info_t* get_plugin_info() const {
        static plugin_info_t info{ nullptr, 0x00010000, 128, 0, 0 };
        return &info;
    }

protected:
    // ── Core notification helpers ─────────────────────────────────────────────

    /// @brief Notify core of a new inbound connection.
    /// @param ep  Filled endpoint descriptor (address, port, flags).
    /// @return Assigned conn_id, or CONN_ID_INVALID on error.
    conn_id_t notify_connect(const endpoint_t* ep) {
        return (api_ && api_->on_connect)
            ? api_->on_connect(api_->ctx, ep) : CONN_ID_INVALID;
    }

    /// @brief Deliver received bytes to the core for frame reassembly.
    void notify_data(conn_id_t id, std::span<const uint8_t> data) {
        if (api_ && api_->on_data)
            api_->on_data(api_->ctx, id, data.data(), data.size());
    }

    /// @brief Notify core that a connection closed.
    /// @param id     Connection ID.
    /// @param error  0 = clean close, non-zero = error code.
    void notify_disconnect(conn_id_t id, int error = 0) {
        if (api_ && api_->on_disconnect)
            api_->on_disconnect(api_->ctx, id, error);
    }

    /// @brief Find conn_id by hex-encoded pubkey.
    conn_id_t find_peer_conn(const char* pubkey_hex) const {
        return (api_ && api_->find_conn_by_pubkey)
            ? api_->find_conn_by_pubkey(api_->ctx, pubkey_hex) : CONN_ID_INVALID;
    }

    /// @brief Get peer endpoint info.
    bool get_peer_info(conn_id_t id, endpoint_t& out) const {
        return api_ && api_->get_peer_info
            && api_->get_peer_info(api_->ctx, id, &out) == 0;
    }

    /// @brief Register an additional handler from within this connector.
    /// @warning Only safe during on_init().
    void register_extra_handler(handler_t* h) {
        if (api_ && api_->register_handler)
            api_->register_handler(api_->ctx, h);
    }

    /// @brief Register a secondary transport path for an existing peer.
    /// @param pubkey_hex  Hex-encoded user pubkey of the peer.
    /// @param ep          Endpoint for the new transport.
    /// @param scheme      Transport scheme (e.g. "ice").
    /// @return transport_conn_id, or CONN_ID_INVALID on error.
    conn_id_t add_transport_path(const char* pubkey_hex,
                                  const endpoint_t* ep, const char* scheme) {
        return (api_ && api_->add_transport)
            ? api_->add_transport(api_->ctx, pubkey_hex, ep, scheme)
            : CONN_ID_INVALID;
    }

    /// @brief Read a config value by dotted key.
    std::string config_get(const char* key) const {
        if (!api_ || !api_->config_get) return {};
        char buf[256]{};
        api_->config_get(api_->ctx, key, buf, sizeof(buf));
        return buf;
    }

    /// @brief Write a log message through the core's logger.
    void log(int level, const char* file, int line, const char* msg) {
        if (api_ && api_->log) api_->log(api_->ctx, level, file, line, msg);
    }
};

} // namespace gn

// ── CONNECTOR_PLUGIN macro ────────────────────────────────────────────────────
/// @brief Generate the plugin entry point for a connector class.
///
/// Creates a file-scope static instance of `ClassName` and exports:
///   - `plugin_get_info()` — metadata accessor
///   - `connector_init(api, out)` — plugin initialization
///
/// @param ClassName  The IConnector subclass to instantiate.
#ifdef GOODNET_STATIC_PLUGINS
#include "../sdk/static_registry.hpp"
#ifndef _GN_CONCAT2
#define _GN_CONCAT2(a, b) a##b
#define _GN_CONCAT(a, b)  _GN_CONCAT2(a, b)
#endif
#define CONNECTOR_PLUGIN(ClassName)                                            \
    static ClassName _gn_connector_instance;                                   \
    static int _gn_static_connector_init(host_api_t* api,                      \
                                         connector_ops_t** out) {              \
        _gn_connector_instance.init(api);                                      \
        *out = _gn_connector_instance.to_c_ops();                              \
        return 0;                                                              \
    }                                                                          \
    namespace { struct _GN_CONCAT(_gn_reg_c_, __LINE__) {                      \
        _GN_CONCAT(_gn_reg_c_, __LINE__)() {                                   \
            gn::static_plugin_registry().push_back(                            \
                {#ClassName, nullptr, _gn_static_connector_init});             \
        }                                                                      \
    } _GN_CONCAT(_gn_reg_c_inst_, __LINE__); }
#else
#define CONNECTOR_PLUGIN(ClassName)                                            \
    static ClassName _gn_connector_instance;                                   \
    extern "C" GN_EXPORT                                                       \
    const plugin_info_t* plugin_get_info() {                                   \
        return _gn_connector_instance.get_plugin_info();                       \
    }                                                                          \
    extern "C" GN_EXPORT                                                       \
    int connector_init(host_api_t* api, connector_ops_t** out) {               \
        _gn_connector_instance.init(api);                                      \
        *out = _gn_connector_instance.to_c_ops();                              \
        return 0;                                                              \
    }
#endif
