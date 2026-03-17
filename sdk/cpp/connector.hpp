#pragma once
/// @file sdk/cpp/connector.hpp
/// @brief C++ base class for transport connector plugins.

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

class IConnector {
protected:
    connector_ops_t ops_{};
    host_api_t*     api_ = nullptr;

public:
    IConnector()  { ops_.connector_ctx = this; }
    virtual ~IConnector() = default;

    IConnector(const IConnector&)            = delete;
    IConnector& operator=(const IConnector&) = delete;

    void init(host_api_t* api) { api_ = api; on_init(); }

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

    // ── Interface ─────────────────────────────────────────────────────────────

    virtual void on_init()     {}
    virtual void on_shutdown() {}

    virtual std::string get_scheme() const = 0;
    virtual std::string get_name()   const = 0;

    /// @return 0 accepted, -1 immediate error
    virtual int  do_connect(const char* uri)                           = 0;
    virtual int  do_listen (const char* host, uint16_t port)           = 0;

    /// Send a contiguous span of bytes. Called when send_gather is not needed.
    virtual int  do_send(conn_id_t id, std::span<const uint8_t> data)  = 0;

    /// Vectored send. Default: iterate and call do_send() per iov segment.
    virtual int  do_send_gather(conn_id_t id, const struct iovec* iov, int n) {
        int rc = 0;
        for (int i = 0; i < n; ++i) {
            rc = do_send(id, {static_cast<const uint8_t*>(iov[i].iov_base),
                               iov[i].iov_len});
            if (rc != 0) break;
        }
        return rc;
    }

    /// @param hard  true = abort immediately, false = graceful drain
    virtual void do_close(conn_id_t id, bool hard)                     = 0;

    virtual const plugin_info_t* get_plugin_info() const {
        static plugin_info_t info{ nullptr, 0x00010000, 128, 0, 0 };
        return &info;
    }

protected:
    // ── Core notification helpers ─────────────────────────────────────────────

    conn_id_t notify_connect(const endpoint_t* ep) {
        return (api_ && api_->on_connect)
            ? api_->on_connect(api_->ctx, ep) : CONN_ID_INVALID;
    }
    void notify_data(conn_id_t id, std::span<const uint8_t> data) {
        if (api_ && api_->on_data)
            api_->on_data(api_->ctx, id, data.data(), data.size());
    }
    void notify_disconnect(conn_id_t id, int error = 0) {
        if (api_ && api_->on_disconnect)
            api_->on_disconnect(api_->ctx, id, error);
    }

    conn_id_t find_peer_conn(const char* pubkey_hex) const {
        return (api_ && api_->find_conn_by_pubkey)
            ? api_->find_conn_by_pubkey(api_->ctx, pubkey_hex) : CONN_ID_INVALID;
    }
    bool get_peer_info(conn_id_t id, endpoint_t& out) const {
        return api_ && api_->get_peer_info
            && api_->get_peer_info(api_->ctx, id, &out) == 0;
    }
    void register_extra_handler(handler_t* h) {
        if (api_ && api_->register_handler)
            api_->register_handler(api_->ctx, h);
    }
    std::string config_get(const char* key) const {
        if (!api_ || !api_->config_get) return {};
        char buf[256]{};
        api_->config_get(api_->ctx, key, buf, sizeof(buf));
        return buf;
    }
    void log(int level, const char* file, int line, const char* msg) {
        if (api_ && api_->log) api_->log(api_->ctx, level, file, line, msg);
    }
};

} // namespace gn

// ── CONNECTOR_PLUGIN macro ────────────────────────────────────────────────────
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
