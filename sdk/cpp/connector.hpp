#pragma once
/// @file sdk/cpp/iconnector.hpp
/// @brief Convenient C++ base class for transport connector plugins.
///
/// Inherit from IConnector, implement the pure-virtual do_* methods, and
/// place CONNECTOR_PLUGIN(MyClass) at the end of your .cpp file.
///
/// ## Reporting events to the core
///
///   notify_connect()    — call after socket accept/async_connect completes
///   notify_data()       — call whenever bytes arrive from the network
///   notify_disconnect() — call when the socket closes or errors
///
/// The conn_id returned by notify_connect() must be stored alongside the
/// socket and passed to notify_data() / notify_disconnect().
///
/// ## Self-registering as a signal handler (ICE pattern)
///
/// Connectors that need a back-channel (e.g. ICE signaling over TCP) can
/// register an additional handler_t during on_init():
///
///   void on_init() override {
///       signal_handler_.name              = "ice_signal_handler";
///       signal_handler_.user_data         = this;
///       signal_handler_.handle_message    = s_handle_signal;
///       signal_handler_.supported_types   = &kSignalType;
///       signal_handler_.num_supported_types = 1;
///       register_extra_handler(&signal_handler_);
///   }
///
/// ## Minimal example
///
///   class MockConnector : public gn::IConnector {
///   public:
///       std::string get_scheme() const override { return "mock"; }
///       std::string get_name()   const override { return "Mock"; }
///       int  do_connect(const char*)              override { return 0; }
///       int  do_listen (const char*, uint16_t)    override { return 0; }
///       int  do_send_to(conn_id_t, const void*, size_t) override { return 0; }
///       void do_close  (conn_id_t)                override {}
///   };
///   CONNECTOR_PLUGIN(MockConnector)

#include "connector.h"

#include <cstring>
#include <string>
#include <span>

namespace gn {

class IConnector {
protected:
    connector_ops_t ops_{};
    host_api_t*     api_ = nullptr;

public:
    IConnector()  { ops_.connector_ctx = this; }
    virtual ~IConnector() = default;

    IConnector(const IConnector&)            = delete;
    IConnector& operator=(const IConnector&) = delete;

    // ── Called by CONNECTOR_PLUGIN macro ─────────────────────────────────────

    void init(host_api_t* api) {
        api_ = api;
        on_init();
    }

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

    // ── Interface for derived classes ─────────────────────────────────────────

    /// Called once on load. Set up io_context, threads, acceptors here.
    virtual void on_init() {}

    /// Called before dlclose().  Close all connections, stop threads.
    virtual void on_shutdown() {}

    /// URI scheme identifier: "tcp", "udp", "ws", "ice", "mock", …
    virtual std::string get_scheme() const = 0;

    /// Human-readable connector name.
    virtual std::string get_name()   const = 0;

    /// Start async outgoing connection. Report via notify_connect() on success.
    /// @return 0 if request accepted, -1 on immediate error
    virtual int  do_connect(const char* uri)                     = 0;

    /// Start listening. Call notify_connect() for each accepted connection.
    /// @return 0 on success, -1 on error
    virtual int  do_listen(const char* host, uint16_t port)      = 0;

    /// Write bytes to conn_id.
    /// @return 0 on success, -1 on error
    virtual int  do_send_to(conn_id_t id, const void* data,
                             size_t size)                         = 0;

    /// Close conn_id. Must eventually call notify_disconnect().
    virtual void do_close(conn_id_t id)                          = 0;

    /// Plugin metadata. Override to set version, priority, caps.
    virtual const plugin_info_t* get_plugin_info() const {
        static plugin_info_t info{ nullptr, 0x00010000, 128, 0, 0 };
        return &info;
    }

protected:

    // ── Core notification helpers ─────────────────────────────────────────────

    /// Register a new connection with the core. Returns conn_id.
    conn_id_t notify_connect(const endpoint_t* ep) {
        return (api_ && api_->on_connect)
            ? api_->on_connect(api_->ctx, ep) : CONN_ID_INVALID;
    }

    /// Forward raw bytes to the core for reassembly and dispatch.
    void notify_data(conn_id_t id, const void* data, size_t size) {
        if (api_ && api_->on_data) api_->on_data(api_->ctx, id, data, size);
    }

    /// Notify the core that a connection has closed.
    void notify_disconnect(conn_id_t id, int error = 0) {
        if (api_ && api_->on_disconnect)
            api_->on_disconnect(api_->ctx, id, error);
    }

    /// Find the conn_id of a currently-established peer by hex-encoded pubkey.
    conn_id_t find_peer_conn(const char* pubkey_hex_64) const {
        return (api_ && api_->find_conn_by_pubkey)
            ? api_->find_conn_by_pubkey(api_->ctx, pubkey_hex_64)
            : CONN_ID_INVALID;
    }

    /// Register an additional handler_t (e.g. ICE signaling back-channel).
    /// Call from on_init() only.
    void register_extra_handler(handler_t* h) {
        if (api_ && api_->register_handler)
            api_->register_handler(api_->ctx, h);
    }

    /// Log via the core's logger.
    void log(int level, const char* file, int line, const char* msg) {
        if (api_ && api_->log)
            api_->log(api_->ctx, level, file, line, msg);
    }
};

} // namespace gn

// ─── CONNECTOR_PLUGIN macro ───────────────────────────────────────────────────
#define CONNECTOR_PLUGIN(ClassName)                                            \
    static ClassName _gn_connector_instance;                                   \
                                                                               \
    extern "C" GN_EXPORT                                                       \
    const plugin_info_t* plugin_get_info() {                                   \
        return _gn_connector_instance.get_plugin_info();                       \
    }                                                                          \
                                                                               \
    extern "C" GN_EXPORT                                                       \
    int connector_init(host_api_t* api, connector_ops_t** out) {               \
        _gn_connector_instance.init(api);                                      \
        *out = _gn_connector_instance.to_c_ops();                              \
        return 0;                                                              \
    }
