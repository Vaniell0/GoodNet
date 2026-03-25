#pragma once
/// @file sdk/cpp/handler.hpp
/// @brief C++ base class for message-handler plugins.
///
/// Provides a type-safe C++ interface over the C `handler_t` ABI.
/// Subclass `gn::IHandler` and implement the pure virtual methods, then
/// use the `HANDLER_PLUGIN(ClassName)` macro to generate the entry point.
///
/// ## Minimal example
/// @code
/// class MyHandler : public gn::IHandler {
/// public:
///     const char* get_plugin_name() const override { return "my_handler"; }
///
///     void on_init() override {
///         set_supported_types({MSG_TYPE_CHAT});
///     }
///
///     void handle_message(const header_t* hdr, const endpoint_t* ep,
///                         std::span<const uint8_t> payload) override {
///         send_response(ep->peer_id, MSG_TYPE_CHAT, payload);
///     }
/// };
/// HANDLER_PLUGIN(MyHandler)
/// @endcode

#include "../sdk/handler.h"
#include <cstring>
#include <initializer_list>
#include <span>
#include <string>
#include <vector>

namespace gn {

/// @brief C++ base class for message-processing handler plugins.
///
/// Wraps the C `handler_t` ABI with type-safe virtual methods and
/// RAII-friendly helper functions for core services.
///
/// Ownership: the handler instance is typically a file-scope static,
/// created by the HANDLER_PLUGIN macro.  Lifetime is managed by
/// the plugin (static storage) — the core holds a raw pointer.
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

    /// @brief Called by the HANDLER_PLUGIN macro.  Stores the host API and
    ///        invokes on_init() for plugin-specific setup.
    void init(host_api_t* api) { api_ = api; on_init(); }

    /// @brief Build the C handler_t descriptor from the virtual interface.
    /// @return Pointer to the internal handler_t (plugin-owned).
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

    // ── Interface (override in subclass) ──────────────────────────────────────

    /// @brief Return the unique plugin name (static string).
    virtual const char* get_plugin_name() const = 0;

    /// @brief Called after the host API is set.  Register supported types here.
    virtual void on_init()    {}

    /// @brief Called before dlclose().  Release all plugin resources.
    virtual void on_shutdown() {}

    /// @brief Process a decrypted packet.
    /// @param header    Wire header.
    /// @param endpoint  Remote peer info (endpoint->peer_id == conn_id).
    /// @param payload   Decrypted payload bytes.
    virtual void handle_message(const header_t*           header,
                                 const endpoint_t*        endpoint,
                                 std::span<const uint8_t> payload) = 0;

    /// @brief Chain-of-responsibility result.  Default: CONTINUE.
    virtual propagation_t on_result(const header_t* /*hdr*/, uint32_t /*type*/) {
        return PROPAGATION_CONTINUE;
    }

    /// @brief Connection state change notification.
    virtual void on_conn_state(const char* /*uri*/, conn_state_t /*state*/) {}

    /// @brief Return plugin metadata.  Override for custom version/priority.
    virtual const plugin_info_t* get_plugin_info() const {
        static plugin_info_t info{ get_plugin_name(), 0x00010000, 128, 0, 0 };
        return &info;
    }

protected:
    /// @brief Set the message types this handler subscribes to.
    /// @param types  Initializer list of MSG_TYPE_* values.
    ///               Empty = wildcard (all types).
    void set_supported_types(std::initializer_list<uint32_t> types) {
        supported_types_.assign(types);
        handler_.supported_types     = supported_types_.data();
        handler_.num_supported_types = supported_types_.size();
    }

    // ── Core API helpers ──────────────────────────────────────────────────────

    /// @brief Send a packet to a peer by URI.
    void send(const char* uri, uint32_t type, std::span<const uint8_t> data) {
        if (api_ && api_->send)
            api_->send(api_->ctx, uri, type, data.data(), data.size());
    }

    /// @brief Send a response on an existing connection.
    void send_response(conn_id_t id, uint32_t type,
                        std::span<const uint8_t> data) {
        if (api_ && api_->send_response)
            api_->send_response(api_->ctx, id, type, data.data(), data.size());
    }

    /// @brief Broadcast to all ESTABLISHED peers.
    void broadcast(uint32_t type, std::span<const uint8_t> data) {
        if (api_ && api_->broadcast)
            api_->broadcast(api_->ctx, type, data.data(), data.size());
    }

    /// @brief Initiate graceful close of a connection.
    void disconnect(conn_id_t id) {
        if (api_ && api_->disconnect)
            api_->disconnect(api_->ctx, id);
    }

    /// @brief Find conn_id by hex-encoded pubkey.
    /// @return conn_id or CONN_ID_INVALID.
    conn_id_t find_conn(const char* pubkey_hex) const {
        return (api_ && api_->find_conn_by_pubkey)
            ? api_->find_conn_by_pubkey(api_->ctx, pubkey_hex) : CONN_ID_INVALID;
    }

    /// @brief Get peer endpoint info.
    /// @return true on success.
    bool get_peer_info(conn_id_t id, endpoint_t& out) const {
        return api_ && api_->get_peer_info
            && api_->get_peer_info(api_->ctx, id, &out) == 0;
    }

    /// @brief Read a config value by dotted key.
    /// @return Config value as string, or empty string if not found.
    std::string config_get(const char* key) const {
        if (!api_ || !api_->config_get) return {};
        char buf[1024]{};
        api_->config_get(api_->ctx, key, buf, sizeof(buf));
        return buf;
    }

    /// @brief Sign data with the node's device key.
    /// @return 0 on success, -1 on error.
    int sign(const void* data, size_t size, uint8_t sig[GN_SIGN_BYTES]) {
        return (api_ && api_->sign_with_device)
            ? api_->sign_with_device(api_->ctx, data, size, sig) : -1;
    }

    /// @brief Verify an Ed25519 signature.
    /// @return 0 if valid, non-zero otherwise.
    int verify(const void* data, size_t size,
                const uint8_t* pk, const uint8_t* sig) {
        return (api_ && api_->verify_signature)
            ? api_->verify_signature(api_->ctx, data, size, pk, sig) : -1;
    }

    /// @brief Write a log message through the core's logger.
    /// @param level  0=trace, 1=debug, 2=info, 3=warn, 4=error, 5=critical.
    void log(int level, const char* file, int line, const char* msg) {
        if (api_ && api_->log) api_->log(api_->ctx, level, file, line, msg);
    }
};

} // namespace gn

// ── HANDLER_PLUGIN macro ──────────────────────────────────────────────────────
/// @brief Generate the plugin entry point for a handler class.
///
/// Creates a file-scope static instance of `ClassName` and exports:
///   - `plugin_get_info()` — metadata accessor
///   - `handler_init(api, out)` — plugin initialization
///
/// @param ClassName  The IHandler subclass to instantiate.
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
