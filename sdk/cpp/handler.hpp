#pragma once
#include "handler.h"
#include <vector>
#include <string>

namespace gn {

/**
 * @brief C++ base class for Handler plugins.
 *
 * Subclass this, override on_init() and handle_message(), then declare
 * the entry point with HANDLER_PLUGIN(MyHandler).
 */
class IHandler {
protected:
    handler_t            handler_{};
    std::vector<uint32_t> supported_types_;
    host_api_t*          api_ = nullptr;

public:
    IHandler() {
        handler_.name              = nullptr;
        handler_.handle_message    = nullptr;
        handler_.handle_conn_state = nullptr;
        handler_.shutdown          = nullptr;
        handler_.supported_types   = nullptr;
        handler_.num_supported_types = 0;
        handler_.user_data         = this;
    }

    virtual ~IHandler() = default;

    void init(host_api_t* api) {
        api_ = api;
        on_init();
    }

    void set_supported_types(const std::vector<uint32_t>& types) {
        supported_types_          = types;
        handler_.supported_types  = supported_types_.data();
        handler_.num_supported_types = supported_types_.size();
    }

    handler_t* to_c_handler() {
        handler_.name = get_plugin_name();

        handler_.handle_message = [](void* ud, const header_t* hdr,
                                     const endpoint_t* ep,
                                     const void* pl, size_t sz) {
            static_cast<IHandler*>(ud)->handle_message(hdr, ep, pl, sz);
        };

        handler_.handle_conn_state = [](void* ud, const char* uri,
                                        conn_state_t st) {
            static_cast<IHandler*>(ud)->handle_connection_state(uri, st);
        };

        handler_.shutdown = [](void* ud) {
            static_cast<IHandler*>(ud)->on_shutdown();
        };

        return &handler_;
    }

    // ── Virtual interface ──────────────────────────────────────────────────

    /**
     * @brief Plugin name exposed to PluginManager for lookup.
     * Override to return a stable string literal.
     */
    virtual const char* get_plugin_name() const = 0;

    /** Called once after api_ is set. Subscribe to types here. */
    virtual void on_init() {}

    /** Called by PluginManager before dlclose(). Flush/cleanup here. */
    virtual void on_shutdown() {}

    virtual void handle_message(const header_t* header,
                                const endpoint_t* endpoint,
                                const void* payload,
                                size_t payload_size) = 0;

    virtual void handle_connection_state(const char* /*uri*/,
                                         conn_state_t /*state*/) {}

protected:
    void send(const char* uri, uint32_t type, const void* data, size_t size) {
        if (api_ && api_->send) api_->send(uri, type, data, size);
    }
};

} // namespace gn
