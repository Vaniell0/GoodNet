#pragma once
/// @file sdk/handler.h
/// @brief C interface for message-processing plugins.
///
/// A handler plugin processes application-layer messages (decrypted payloads).
/// The core dispatches packets through a priority-ordered handler chain.
///
/// ## Plugin lifecycle
///   1. Core calls `handler_init(api, &handler)` with the host API.
///   2. Plugin fills a `handler_t` struct and returns 0 on success.
///   3. Core registers the handler and begins dispatching matching packets.
///   4. On shutdown, core calls `handler->shutdown(user_data)`.
///   5. Core calls `dlclose()` — all plugin memory becomes invalid.
///
/// ## Dispatch pipeline
///   Handlers are invoked in descending priority order (`plugin_info->priority`,
///   0 = highest).  For each packet:
///     1. Core calls `handle_message()`.
///     2. Core calls `on_message_result()` (if non-NULL) to get a propagation decision.
///     3. PROPAGATION_CONTINUE -> next handler.  CONSUMED -> stop + pin affinity.
///        REJECT -> stop + drop silently.
///
/// ## Session affinity
///   When a handler returns CONSUMED, subsequent packets on the same connection
///   skip lower-priority handlers and go directly to the pinned handler.

#include "plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Handler descriptor registered with the core.
///
/// The core stores a raw pointer to this struct — the plugin owns the lifetime.
/// Typical pattern: static instance filled during `handler_init()`.
///
/// All callbacks receive `user_data` as their first argument — typically `this`
/// for C++ plugins or a pointer to the plugin's state struct for C plugins.
struct handler_t {

    /// @brief Unique handler name.
    ///        Used as key in PluginManager and SignalBus.
    ///        Must point to a static string — the core does not copy it.
    const char* name;

    /// @brief Core packet callback — invoked for each matching decrypted packet.
    ///
    /// @param user_data  Plugin context (handler_t::user_data).
    /// @param header     Wire header (read-only, valid for the duration of the call).
    /// @param endpoint   Remote peer info.  `endpoint->peer_id` == conn_id.
    ///                   Use `api->send_response(endpoint->peer_id, ...)` to reply.
    /// @param payload    Decrypted payload bytes (valid for the duration of the call).
    /// @param payload_size  Payload byte count.
    void (*handle_message)(void*             user_data,
                           const header_t*   header,
                           const endpoint_t* endpoint,
                           const void*       payload,
                           size_t            payload_size);

    /// @brief Chain-of-responsibility hook (optional).
    ///
    /// Called by the core immediately after `handle_message()` returns.
    /// Controls dispatch pipeline propagation:
    ///   - PROPAGATION_CONTINUE: pass to next handler
    ///   - PROPAGATION_CONSUMED: stop chain, pin session affinity
    ///   - PROPAGATION_REJECT:   drop packet silently
    ///
    /// If NULL, the core treats this handler as always returning CONTINUE.
    ///
    /// @param user_data  Plugin context.
    /// @param header     Wire header.
    /// @param msg_type   Message type (same as header->payload_type).
    /// @return Propagation decision.
    propagation_t (*on_message_result)(void*           user_data,
                                        const header_t* header,
                                        uint32_t        msg_type);

    /// @brief Connection state change notification.
    ///
    /// Called when a connection transitions between states (see conn_state_t).
    /// Typically used to track peer presence or clean up per-connection state.
    ///
    /// @param user_data  Plugin context.
    /// @param uri        Hex-encoded peer pubkey (64 chars) or address string.
    /// @param state      New connection state.
    void (*handle_conn_state)(void*        user_data,
                               const char*  uri,
                               conn_state_t state);

    /// @brief Shutdown callback — release all resources.
    ///
    /// Called by the core before `dlclose()`.  After this returns, no further
    /// callbacks will be invoked and the plugin's address space becomes invalid.
    ///
    /// @param user_data  Plugin context.
    void (*shutdown)(void* user_data);

    /// @brief Message types this handler subscribes to.
    ///
    /// If NULL or num_supported_types == 0, the handler receives ALL message types
    /// (wildcard subscription).  Otherwise, only listed types are dispatched.
    ///
    /// Array is owned by the plugin — the core reads but never frees it.
    const uint32_t* supported_types;
    size_t          num_supported_types;  ///< Element count of supported_types[]

    /// @brief Plugin self-description.
    ///        NULL -> PluginManager assigns default metadata.
    const plugin_info_t* info;

    /// @brief Opaque plugin context passed to every callback.
    ///        For C++ plugins, typically `this`.
    void* user_data;
};

/// @brief Handler plugin entry point.
///
/// Exported as `handler_init` (with `GN_EXPORT`).  The core calls this once
/// during plugin loading.  The plugin MUST:
///   1. Use `api` for all core interactions.
///   2. Fill `*out_handler` with a pointer to a valid `handler_t`.
///   3. Return 0 on success, non-zero on failure.
///
/// @param api          Host API vtable (valid for plugin lifetime).
/// @param out_handler  Output: plugin-owned handler descriptor.
/// @return 0 on success, non-zero on failure.
typedef int (*handler_init_t)(host_api_t* api, handler_t** out_handler);

#ifdef __cplusplus
}
#endif
