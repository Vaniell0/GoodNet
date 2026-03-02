#pragma once
#include "plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

// Текущая версия API — плагин должен вернуть это значение в api_version
#define EXPECTED_API_VERSION 1

#pragma pack(push, 8) // Выравнивание по 8 байт (стандарт для 64-бит)
// Структура обработчика
typedef struct {
    // --- Метаданные (заполняет плагин, статические строки) ---
    uint32_t    api_version;    // Должно быть == EXPECTED_API_VERSION
    const char* name;           // Имя обработчика, например "auth_handler"

    // --- Lifecycle ---
    void (*shutdown)(void* user_data);  // Вызывается перед dlclose (может быть NULL)

    // --- Функции обработки (указатели) ---
    void (*handle_message)(void* user_data,
                           const header_t*   header,
                           const endpoint_t* endpoint,
                           const void*       payload,
                           size_t            payload_size);

    void (*handle_conn_state)(void* user_data,
                              const char*   uri,
                              conn_state_t  state);

    // --- Поддерживаемые типы сообщений ---
    uint32_t* supported_types;
    size_t    num_supported_types;

    // --- Пользовательские данные ---
    void* user_data;
} handler_t;
#pragma pack(pop)

// Точка входа плагина-обработчика:
//   extern "C" handler_t* get_handler(host_api_t* api);
typedef handler_t* (*get_handler_fn_t)(host_api_t* api);

#ifdef __cplusplus
}
#endif
