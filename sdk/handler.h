#pragma once
#include "plugin.h"

#ifdef __cplusplus
extern "C" {
#endif


// Структура обработчика
typedef struct {
    // Функции обработки (указатели)
    void (*handle_message)(void* user_data, const header_t* header,
                          const endpoint_t* endpoint, const void* payload,
                          size_t payload_size);
    void (*handle_conn_state)(void* user_data, const char* uri,
                             conn_state_t state);
    
    // Поддерживаемые типы сообщений
    uint32_t* supported_types;
    size_t num_supported_types;
    
    // Пользовательские данные
    void* user_data;
} handler_t;

// Сигнатура функции инициализации обработчика
typedef int (*handler_init_t)(host_api_t* api, handler_t** handler);

#ifdef __cplusplus
}
#endif