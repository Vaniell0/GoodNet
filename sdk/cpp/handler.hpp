#pragma once
/// @file sdk/cpp/handler.hpp
/// @brief C++ base class for message-handler plugins.

#include "../sdk/handler.h"
#include <cstring>
#include <initializer_list>
#include <span>
#include <string>
#include <vector>

namespace gn {

class IHandler {
protected:
    handler_t             handler_{};
    std::vector<uint32_t> supported_types_;
    host_api_t*           api_ = nullptr;

public:
    IHandler() { handler_.user_data = this; }
    virtual ~IHandler() = default;

    IHandler(const IHandler&)            = delete;
    IHandler& operator=(const IHandler&) = delete;

    void init(host_api_t* api) { api_ = api; on_init(); }

    handler_t* to_c_handler() {
        handler_.name = get_plugin_name();
        handler_.info = get_plugin_info();

        handler_.handle_message = [](void* ud, const header_t* h,
                                      const endpoint_t* ep,
                                      const void* pl, size_t sz) {
            static_cast<IHandler*>(ud)->handle_message(
                h, ep, std::span<const uint8_t>(
                    static_cast<const uint8_t*>(pl), sz));
        };
        handler_.on_message_result = [](void* ud, const header_t* h,
                                         uint32_t t) -> propagation_t {
            return static_cast<IHandler*>(ud)->on_result(h, t);
        };
        handler_.handle_conn_state = [](void* ud, const char* uri,
                                         conn_state_t st) {
            static_cast<IHandler*>(ud)->on_conn_state(uri, st);
        };
        handler_.shutdown = [](void* ud) {
            static_cast<IHandler*>(ud)->on_shutdown();
        };
        handler_.supported_types     = supported_types_.data();
        handler_.num_supported_types = supported_types_.size();
        return &handler_;
    }

    // ── Interface ─────────────────────────────────────────────────────────────

    virtual const char* get_plugin_name() const = 0;
    virtual void on_init()    {}
    virtual void on_shutdown() {}

    /// Core packet callback. ep->peer_id == conn_id for send_response().
    virtual void handle_message(const header_t*           header,
                                 const endpoint_t*        endpoint,
                                 std::span<const uint8_t> payload) = 0;

    /// Chain result after handle_message(). Default: CONTINUE.
    virtual propagation_t on_result(const header_t* /*hdr*/, uint32_t /*type*/) {
        return PROPAGATION_CONTINUE;
    }

    /// Connection state change notification.
    virtual void on_conn_state(const char* /*uri*/, conn_state_t /*state*/) {}

    virtual const plugin_info_t* get_plugin_info() const {
        static plugin_info_t info{ get_plugin_name(), 0x00010000, 128, 0, 0 };
        return &info;
    }

protected:
    void set_supported_types(std::initializer_list<uint32_t> types) {
        supported_types_.assign(types);
        handler_.supported_types     = supported_types_.data();
        handler_.num_supported_types = supported_types_.size();
    }

    // ── Core API helpers ──────────────────────────────────────────────────────

    void send(const char* uri, uint32_t type, std::span<const uint8_t> data) {
        if (api_ && api_->send)
            api_->send(api_->ctx, uri, type, data.data(), data.size());
    }
    void send_response(conn_id_t id, uint32_t type,
                        std::span<const uint8_t> data) {
        if (api_ && api_->send_response)
            api_->send_response(api_->ctx, id, type, data.data(), data.size());
    }
    void broadcast(uint32_t type, std::span<const uint8_t> data) {
        if (api_ && api_->broadcast)
            api_->broadcast(api_->ctx, type, data.data(), data.size());
    }
    void disconnect(conn_id_t id) {
        if (api_ && api_->disconnect)
            api_->disconnect(api_->ctx, id);
    }

    conn_id_t find_conn(const char* pubkey_hex) const {
        return (api_ && api_->find_conn_by_pubkey)
            ? api_->find_conn_by_pubkey(api_->ctx, pubkey_hex) : CONN_ID_INVALID;
    }
    bool get_peer_info(conn_id_t id, endpoint_t& out) const {
        return api_ && api_->get_peer_info
            && api_->get_peer_info(api_->ctx, id, &out) == 0;
    }
    std::string config_get(const char* key) const {
        if (!api_ || !api_->config_get) return {};
        char buf[256]{};
        api_->config_get(api_->ctx, key, buf, sizeof(buf));
        return buf;
    }

    int sign(const void* data, size_t size, uint8_t sig[64]) {
        return (api_ && api_->sign_with_device)
            ? api_->sign_with_device(api_->ctx, data, size, sig) : -1;
    }
    int verify(const void* data, size_t size,
                const uint8_t* pk, const uint8_t* sig) {
        return (api_ && api_->verify_signature)
            ? api_->verify_signature(api_->ctx, data, size, pk, sig) : -1;
    }

    void log(int level, const char* file, int line, const char* msg) {
        if (api_ && api_->log) api_->log(api_->ctx, level, file, line, msg);
    }
};

} // namespace gn

// ── HANDLER_PLUGIN macro ──────────────────────────────────────────────────────
#ifdef GOODNET_STATIC_PLUGINS
#include "../sdk/static_registry.hpp"
#define _GN_CONCAT2(a, b) a##b
#define _GN_CONCAT(a, b)  _GN_CONCAT2(a, b)
#define HANDLER_PLUGIN(ClassName)                                              \
    static ClassName _gn_plugin_instance;                                      \
    static int _gn_static_handler_init(host_api_t* api, handler_t** out) {     \
        _gn_plugin_instance.init(api);                                         \
        *out = _gn_plugin_instance.to_c_handler();                             \
        return 0;                                                              \
    }                                                                          \
    namespace { struct _GN_CONCAT(_gn_reg_h_, __LINE__) {                      \
        _GN_CONCAT(_gn_reg_h_, __LINE__)() {                                   \
            gn::static_plugin_registry().push_back(                            \
                {#ClassName, _gn_static_handler_init, nullptr});               \
        }                                                                      \
    } _GN_CONCAT(_gn_reg_h_inst_, __LINE__); }
#else
#define HANDLER_PLUGIN(ClassName)                                              \
    static ClassName _gn_plugin_instance;                                      \
    extern "C" GN_EXPORT                                                       \
    const plugin_info_t* plugin_get_info() {                                   \
        return _gn_plugin_instance.get_plugin_info();                          \
    }                                                                          \
    extern "C" GN_EXPORT                                                       \
    int handler_init(host_api_t* api, handler_t** out) {                       \
        _gn_plugin_instance.init(api);                                         \
        *out = _gn_plugin_instance.to_c_handler();                             \
        return 0;                                                              \
    }
#endif
