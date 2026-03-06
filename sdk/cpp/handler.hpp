#pragma once
#include "../handler.h"
#include <vector>
#include <string>
#include <cstring>

namespace gn {

/// @defgroup ihandler IHandler C++ Base Class
/// @brief Convenient C++ base for handler plugins.
///
/// Inherit from IHandler, override virtual methods, add HANDLER_PLUGIN(MyClass)
/// at the end of the .cpp file.
///
/// ## Minimal example
/// ```cpp
/// class PingHandler : public gn::IHandler {
/// public:
///     const char* get_plugin_name() const override { return "ping"; }
///     void on_init() override { set_supported_types({MSG_TYPE_HEARTBEAT}); }
///     void handle_message(const header_t*, const endpoint_t* ep,
///                         const void* pl, size_t sz) override {
///         send(ep->address, MSG_TYPE_HEARTBEAT, pl, sz);  // echo back
///     }
/// };
/// HANDLER_PLUGIN(PingHandler)
/// ```
/// @{
class IHandler {
protected:
    handler_t             handler_{};
    std::vector<uint32_t> supported_types_;
    host_api_t*           api_ = nullptr;

public:
    IHandler() { handler_.user_data = this; }
    virtual ~IHandler() = default;

    /// @brief Called by HANDLER_PLUGIN macro. Do not override.
    void init(host_api_t* api) { api_ = api; on_init(); }

    /// @brief Populate handler_ C callbacks and return the descriptor pointer.
    handler_t* to_c_handler() {
        handler_.name = get_plugin_name();

        handler_.handle_message = [](void* ud, const header_t* h,
                                      const endpoint_t* ep,
                                      const void* pl, size_t sz) {
            static_cast<IHandler*>(ud)->handle_message(h, ep, pl, sz);
        };
        handler_.handle_conn_state = [](void* ud, const char* uri, conn_state_t st) {
            static_cast<IHandler*>(ud)->handle_connection_state(uri, st);
        };
        handler_.shutdown = [](void* ud) {
            static_cast<IHandler*>(ud)->on_shutdown();
        };

        handler_.supported_types     = supported_types_.data();
        handler_.num_supported_types = supported_types_.size();
        return &handler_;
    }

    // ── Interface for derived classes ──────────────────────────────────────

    /// @brief Unique plugin name — key for PluginManager::find_handler_by_name().
    virtual const char* get_plugin_name() const = 0;

    /// @brief Called once on load. Call set_supported_types() here.
    virtual void on_init() {}

    /// @brief Called for each fully-assembled, decrypted packet.
    virtual void handle_message(const header_t*   header,
                                 const endpoint_t* endpoint,
                                 const void*       payload,
                                 size_t            payload_size) = 0;

    /// @brief Called when a connection changes state (optional).
    virtual void handle_connection_state(const char* /*uri*/,
                                          conn_state_t /*state*/) {}

    /// @brief Called before dlclose(). Release all resources.
    virtual void on_shutdown() {}

protected:
    /// @brief Subscribe to specific message types.
    ///        Call with an empty list to receive all types (wildcard).
    void set_supported_types(std::initializer_list<uint32_t> types) {
        supported_types_.assign(types);
        handler_.supported_types     = supported_types_.data();
        handler_.num_supported_types = supported_types_.size();
    }

    /// @brief Send a packet through ConnectionManager.
    void send(const char* uri, uint32_t type, const void* data, size_t size) {
        if (api_ && api_->send)
            api_->send(api_->ctx, uri, type, data, size);
    }

    /// @brief Sign buffer with the core's device Ed25519 secret key.
    /// @return 0 on success
    int sign(const void* data, size_t size, uint8_t sig[64]) {
        if (!api_ || !api_->sign_with_device) return -1;
        return api_->sign_with_device(api_->ctx, data, size, sig);
    }

    /// @brief Verify an Ed25519 signature.
    /// @return 0 if valid
    int verify(const void* data, size_t size,
               const uint8_t* pubkey, const uint8_t* sig) {
        if (!api_ || !api_->verify_signature) return -1;
        return api_->verify_signature(api_->ctx, data, size, pubkey, sig);
    }
};

/// @}  // defgroup ihandler

} // namespace gn
