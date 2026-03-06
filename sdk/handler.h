#pragma once
#include "plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @defgroup handler Handler Plugin C API
/// @brief C interface for message-processing plugins.
/// @{

/// @brief Handler descriptor passed to the core via handler_init().
///
/// The core stores a raw pointer — the plugin owns the lifetime.
/// Typical pattern: static variable inside handler_init() (Meyers singleton).
///
/// Workflow:
/// 1. Plugin calls handler_init() → fills handler_t → returns pointer via out.
/// 2. Core calls handle_message() for each packet from ESTABLISHED connections
///    whose payload_type is in supported_types[].
/// 3. Core calls shutdown() before dlclose().
typedef struct {

    /// @brief Unique handler name, used as key in find_handler_by_name().
    ///        Must point to a static string inside the plugin (not copied).
    const char* name;

    /// @brief Invoked for each fully-assembled, decrypted packet.
    /// @param user_data  Opaque pointer (typically `this`)
    /// @param header     Validated, decrypted packet header
    /// @param endpoint   Remote peer address and pubkey
    /// @param payload    Decrypted payload bytes
    /// @param payload_size Byte length of payload
    void (*handle_message)(void*             user_data,
                           const header_t*   header,
                           const endpoint_t* endpoint,
                           const void*       payload,
                           size_t            payload_size);

    /// @brief Invoked when connection state changes.
    /// @param user_data  Opaque pointer
    /// @param uri        Connection identifier ("ip:port" or peer pubkey hex)
    /// @param state      New conn_state_t value
    void (*handle_conn_state)(void*        user_data,
                              const char*  uri,
                              conn_state_t state);

    /// @brief Invoked by the core before dlclose(). Release all resources here.
    void (*shutdown)(void* user_data);

    /// @brief Message types this handler subscribes to.
    ///        NULL or num_supported_types == 0 → wildcard (receives all types).
    ///        The array is owned by the plugin; the core never frees it.
    const uint32_t* supported_types;
    size_t          num_supported_types;

    /// @brief Opaque plugin context, typically `this` for C++ handlers.
    void* user_data;

} handler_t;

/// @brief Handler plugin entry point signature.
/// @param api        Host API provided by the core
/// @param out_handler Output: pointer to the handler descriptor
/// @return 0 on success, non-zero on failure
typedef int (*handler_init_t)(host_api_t* api, handler_t** out_handler);

/// @}  // defgroup handler

#ifdef __cplusplus
}
#endif
