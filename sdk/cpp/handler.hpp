#pragma once
/// @file sdk/cpp/ihandler.hpp
/// @brief Convenient C++ base class for handler plugins.
///
/// Inherit from IHandler, override the virtual methods below, and place
/// HANDLER_PLUGIN(MyClass) at the end of your .cpp file.
///
/// ## Pipeline integration
///
/// The core calls handle_message() then on_result() for every packet routed
/// to this handler.  on_result() drives the chain-of-responsibility:
///
///   PROPAGATION_CONTINUE  — packet passes to the next lower-priority handler.
///   PROPAGATION_CONSUMED  — chain stops; this handler's name is pinned as the
///                           session affinity for this connection.
///   PROPAGATION_REJECT    — packet dropped silently.
///
/// Default on_result() returns PROPAGATION_CONTINUE so simple handlers that
/// don't care about the chain need not override it.
///
/// ## Responding to messages
///
/// Inside handle_message(), call send_response() with endpoint->peer_id to
/// reply directly on the same connection — no URI lookup needed:
///
///   void handle_message(const header_t* h, const endpoint_t* ep,
///                       const void* pl, size_t sz) override {
///       send_response(ep->peer_id, MSG_TYPE_HEARTBEAT, pl, sz);
///   }
///
/// ## Minimal example
///
///   class EchoHandler : public gn::IHandler {
///   public:
///       const char* get_plugin_name() const override { return "echo"; }
///       void on_init() override { set_supported_types({MSG_TYPE_CHAT}); }
///
///       void handle_message(const header_t*, const endpoint_t* ep,
///                           const void* pl, size_t sz) override {
///           send_response(ep->peer_id, MSG_TYPE_CHAT, pl, sz);
///       }
///
///       propagation_t on_result(uint32_t) override {
///           return PROPAGATION_CONSUMED;  // no other handler needed
///       }
///   };
///   HANDLER_PLUGIN(EchoHandler)

#include "handler.h"

#include <vector>
#include <string>
#include <cstring>
#include <initializer_list>

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

    // ── Called by HANDLER_PLUGIN macro — do not override ─────────────────────

    void init(host_api_t* api) {
        api_ = api;
        on_init();
    }

    /// Build the C handler_t and return a pointer to it.
    handler_t* to_c_handler() {
        handler_.name = get_plugin_name();
        handler_.info = get_plugin_info();

        handler_.handle_message = [](void* ud, const header_t* h,
                                      const endpoint_t* ep,
                                      const void* pl, size_t sz) {
            static_cast<IHandler*>(ud)->handle_message(h, ep, pl, sz);
        };

        handler_.on_message_result = [](void* ud, const header_t* h,
                                         uint32_t msg_type) -> propagation_t {
            return static_cast<IHandler*>(ud)->on_result(h, msg_type);
        };

        handler_.handle_conn_state = [](void* ud, const char* uri,
                                         conn_state_t st) {
            static_cast<IHandler*>(ud)->on_connection_state(uri, st);
        };

        handler_.shutdown = [](void* ud) {
            static_cast<IHandler*>(ud)->on_shutdown();
        };

        handler_.supported_types      = supported_types_.data();
        handler_.num_supported_types  = supported_types_.size();

        return &handler_;
    }

    // ── Interface for derived classes ─────────────────────────────────────────

    /// Unique plugin name — key in PluginManager and SignalBus.
    virtual const char* get_plugin_name() const = 0;

    /// Called once on load.  Call set_supported_types() here.
    virtual void on_init() {}

    /// Core packet callback.  endpoint->peer_id == conn_id for send_response().
    virtual void handle_message(const header_t*   header,
                                 const endpoint_t* endpoint,
                                 const void*       payload,
                                 size_t            payload_size) = 0;

    /// Chain result after handle_message().  Default: CONTINUE.
    virtual propagation_t on_result(const header_t* /*header*/,
                                     uint32_t        /*msg_type*/) {
        return PROPAGATION_CONTINUE;
    }

    /// Connection state change notification (optional).
    virtual void on_connection_state(const char* /*uri*/,
                                      conn_state_t /*state*/) {}

    /// Called before dlclose() — release all resources.
    virtual void on_shutdown() {}

    /// Plugin metadata.  Override to set version, priority, caps.
    virtual const plugin_info_t* get_plugin_info() const {
        static plugin_info_t info{ get_plugin_name(), 0x00010000, 128, 0, 0 };
        return &info;
    }

protected:

    /// Subscribe to specific message types (call from on_init()).
    void set_supported_types(std::initializer_list<uint32_t> types) {
        supported_types_.assign(types);
        handler_.supported_types     = supported_types_.data();
        handler_.num_supported_types = supported_types_.size();
    }

    // ── Core API helpers ──────────────────────────────────────────────────────

    /// Send to a peer by URI (connects if not yet established).
    void send(const char* uri, uint32_t type, const void* data, size_t size) {
        if (api_ && api_->send)
            api_->send(api_->ctx, uri, type, data, size);
    }

    /// Reply on an existing connection — use endpoint->peer_id from handle_message().
    void send_response(conn_id_t conn_id, uint32_t type,
                        const void* data, size_t size) {
        if (api_ && api_->send_response)
            api_->send_response(api_->ctx, conn_id, type, data, size);
    }

    /// Find the conn_id for a currently-connected peer by hex pubkey.
    conn_id_t find_conn(const char* pubkey_hex_64) const {
        if (api_ && api_->find_conn_by_pubkey)
            return api_->find_conn_by_pubkey(api_->ctx, pubkey_hex_64);
        return CONN_ID_INVALID;
    }

    /// Sign with the node's device Ed25519 key.  Returns 0 on success.
    int sign(const void* data, size_t size, uint8_t sig[64]) {
        return (api_ && api_->sign_with_device)
            ? api_->sign_with_device(api_->ctx, data, size, sig) : -1;
    }

    /// Verify an Ed25519 signature.  Returns 0 if valid.
    int verify(const void* data, size_t size,
                const uint8_t* pubkey, const uint8_t* sig) {
        return (api_ && api_->verify_signature)
            ? api_->verify_signature(api_->ctx, data, size, pubkey, sig) : -1;
    }

    /// Log via the core's logger (level: 0=trace … 5=critical).
    void log(int level, const char* file, int line, const char* msg) {
        if (api_ && api_->log)
            api_->log(api_->ctx, level, file, line, msg);
    }
};

} // namespace gn

// ─── HANDLER_PLUGIN macro ─────────────────────────────────────────────────────
/// Place at the end of your plugin's .cpp file.
///
///   HANDLER_PLUGIN(MyHandlerClass)
///
/// Exposes the C entry points handler_init() and optionally plugin_get_info().
#define HANDLER_PLUGIN(ClassName)                                              \
    static ClassName _gn_plugin_instance;                                      \
                                                                               \
    extern "C" GN_EXPORT                                                       \
    const plugin_info_t* plugin_get_info() {                                   \
        return _gn_plugin_instance.get_plugin_info();                          \
    }                                                                          \
                                                                               \
    extern "C" GN_EXPORT                                                       \
    int handler_init(host_api_t* api, handler_t** out) {                       \
        _gn_plugin_instance.init(api);                                         \
        *out = _gn_plugin_instance.to_c_handler();                             \
        return 0;                                                              \
    }
