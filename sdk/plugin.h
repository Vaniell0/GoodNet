#pragma once
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GNET_API_VERSION 1

typedef struct {
    // Версия API хоста
    uint32_t api_version;
    
    // Функции хоста
    void (*send)(const char* uri, uint32_t type, const void* data, size_t size);
    handle_t (*create_connection)(const char* uri);
    void (*close_connection)(handle_t handle);
    void (*update_connection_state)(const char* uri, conn_state_t state);
    
    plugin_type_t plugin_type;
} host_api_t;

#ifdef __cplusplus
}
#endif
