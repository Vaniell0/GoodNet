#pragma once

#include "../sdk/handler.h"
#include "../sdk/connector.h"

/* МАКРОС ДЛЯ ОБРАБОТЧИКОВ */
#define HANDLER_PLUGIN(ClassName) \
extern "C"  int handler_init(host_api_t* api, handler_t** handler) { \
    static ClassName instance; \
    if (api && api->plugin_type == PLUGIN_TYPE_HANDLER) { \
        instance.init(api); \
        *handler = instance.to_c_handler(); \
        return 0; \
    } \
    return -1; \
}

/* МАКРОС ДЛЯ КОННЕКТОРОВ */
#define CONNECTOR_PLUGIN(ClassName) \
extern "C" int connector_init(host_api_t* api, connector_ops_t** ops) { \
    static ClassName instance; \
    if (api && api->plugin_type == PLUGIN_TYPE_CONNECTOR) { \
        instance.init(api); \
        *ops = instance.to_c_ops(); \
        return 0; \
    } \
    return -1; \
}
