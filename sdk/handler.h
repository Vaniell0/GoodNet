#pragma once
/// @file sdk/handler.h
/// @brief C interface for message-processing plugins.

#include "plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Handler descriptor registered with the core via handler_init().
///
/// The core stores a raw pointer — the plugin owns the lifetime.
/// Typical pattern: static instance inside handler_init() (Meyers singleton).
///
/// Dispatch pipeline
/// ─────────────────
/// Handlers are invoked in descending priority order (plugin_info->priority).
/// After handle_message() returns, the core calls on_message_result() if set.
/// Returning PROPAGATION_CONSUMED stops the chain and pins session affinity
/// (subsequent packets on this connection skip lower-priority handlers).
/// NULL on_message_result → treated as PROPAGATION_CONTINUE.
struct handler_t {

    /// Unique handler name — key for PluginManager and SignalBus.
    /// Must point to a static string inside the plugin.
    const char* name;

    /// @brief Invoked for each fully-assembled, decrypted packet.
    ///
    /// endpoint->peer_id == conn_id for this packet.
    /// Use api->send_response(endpoint->peer_id, ...) to reply without a URI.
    void (*handle_message)(void*             user_data,
                           const header_t*   header,
                           const endpoint_t* endpoint,
                           const void*       payload,
                           size_t            payload_size);

    /// @brief Chain-of-responsibility hook (optional).
    ///        Called by the core immediately after handle_message() returns.
    ///        Return PROPAGATION_CONSUMED to stop further dispatch.
    ///        Return PROPAGATION_REJECT  to silently drop the packet.
    ///        NULL → the bus treats this handler as always returning CONTINUE.
    propagation_t (*on_message_result)(void*           user_data,
                                        const header_t* header,
                                        uint32_t        msg_type);

    /// @brief Invoked when a connection changes state.
    void (*handle_conn_state)(void*        user_data,
                               const char*  uri,
                               conn_state_t state);

    /// @brief Invoked by the core before dlclose().  Release all resources.
    void (*shutdown)(void* user_data);

    /// @brief Message types subscribed to.
    ///        NULL / num == 0 → wildcard (receives all types).
    ///        Array owned by the plugin; never freed by the core.
    const uint32_t* supported_types;
    size_t          num_supported_types;

    /// @brief Plugin self-description.  NULL → PluginManager assigns defaults.
    const plugin_info_t* info;

    /// Opaque plugin context, typically `this` for C++ plugins.
    void* user_data;
};

/// @brief Handler plugin entry point.
typedef int (*handler_init_t)(host_api_t* api, handler_t** out_handler);

/// @brief Optional metadata export called before handler_init().
///        Export this symbol to let PluginManager read metadata without init.
typedef const plugin_info_t* (*plugin_get_info_t)(void);

/// @brief Handler plugin entry point signature.
/// @param api        Host API provided by the core
/// @param out_handler Output: pointer to the handler descriptor
/// @return 0 on success, non-zero on failure
typedef int (*handler_init_t)(host_api_t* api, handler_t** out_handler);

/// @}  // defgroup handler

#ifdef __cplusplus
}
#endif