#pragma once

#include "../sdk/handler.h"
#include "../sdk/connector.h"

#ifdef __GNUC__
#  define PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#  define PLUGIN_EXPORT
#endif

/* МАКРОС ДЛЯ ОБРАБОТЧИКОВ */
#define HANDLER_PLUGIN(ClassName) \
extern "C" PLUGIN_EXPORT int handler_init(host_api_t* api, handler_t** handler) { \
    static ClassName instance; \
    if (api && api->plugin_type == PLUGIN_TYPE_HANDLER) { \
        if (api->api_version != GNET_API_VERSION) { \
            return -1; \
        } \
        instance.init(api); \
        *handler = instance.to_c_handler(); \
        return 0; \
    } \
    return -1; \
}

/* МАКРОС ДЛЯ КОННЕКТОРОВ */
#define CONNECTOR_PLUGIN(ClassName) \
extern "C" PLUGIN_EXPORT int connector_init(host_api_t* api, connector_ops_t** ops) { \
    static ClassName instance; \
    if (api && api->plugin_type == PLUGIN_TYPE_CONNECTOR) { \
        if (api->api_version != GNET_API_VERSION) { \
            return -1; \
        } \
        instance.init(api); \
        *ops = instance.to_c_ops(); \
        return 0; \
    } \
    return -1; \
}
