#pragma once
#include "plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

// Колбэки соединения
typedef struct {
    void (*on_data)(void* user_data, const void* data, size_t size);
    void (*on_close)(void* user_data);
    void (*on_error)(void* user_data, int error_code);
    void* user_data;
} connection_callbacks_t;

// Функции соединения
typedef struct {
    // Указатели на функции
    int (*send)(void* conn_ctx, const void* data, size_t size);
    int (*close)(void* conn_ctx);
    int (*is_active)(void* conn_ctx);
    void (*get_endpoint)(void* conn_ctx, endpoint_t* endpoint);
    void (*get_uri)(void* conn_ctx, char* buffer, size_t size);
    void (*set_callbacks)(void* conn_ctx, const connection_callbacks_t* callbacks);
    
    // Контекст соединения
    void* conn_ctx;
} connection_ops_t;

// Функции коннектора
typedef struct {
    // Указатели на функции
    connection_ops_t* (*connect)(void* connector_ctx, const char* uri);
    int (*listen)(void* connector_ctx, const char* host, uint16_t port);
    void (*get_scheme)(void* connector_ctx, char* scheme, size_t size);
    void (*get_name)(void* connector_ctx, char* name, size_t size);
    void (*shutdown)(void* connector_ctx);
    
    // Контекст коннектора
    void* connector_ctx;
} connector_ops_t;

// Сигнатура функции инициализации коннектора
typedef int (*connector_init_t)(host_api_t* api, connector_ops_t** ops);

#ifdef __cplusplus
}
#endif