#pragma once
#include "plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Handler plugin descriptor.
 *
 * Returned by handler_init() and stored by PluginManager.
 * All function pointers except handle_message are optional (may be null).
 *
 * Lifetime: owned by the plugin's static instance. PluginManager holds a
 * non-owning pointer; it must not call free() on this struct.
 */
typedef struct {
    /**
     * Human-readable name used for lookup via find_handler_by_name().
     * Optional: if NULL, PluginManager falls back to the .so stem.
     */
    const char* name;

    /**
     * Called for each incoming packet whose payload_type is in supported_types
     * (or for every packet if num_supported_types == 0). Required.
     */
    void (*handle_message)(void* user_data, const header_t* header,
                           const endpoint_t* endpoint, const void* payload,
                           size_t payload_size);

    /** Called when a connection changes state. Optional. */
    void (*handle_conn_state)(void* user_data, const char* uri,
                              conn_state_t state);

    /** Called by PluginManager before dlclose(). Optional. */
    void (*shutdown)(void* user_data);

    /** Array of accepted payload_type values. NULL / 0 = accept all. */
    uint32_t* supported_types;
    size_t    num_supported_types;

    /** Passed as first argument to every callback. Typically `this`. */
    void* user_data;
} handler_t;

#ifdef __cplusplus
}
#endif
