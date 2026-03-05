#pragma once
#include "plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

// ─── handler_t ────────────────────────────────────────────────────────────────
//
// C-структура, которую плагин-хендлер передаёт ядру через handler_init().
// Ядро хранит указатель на эту структуру — плагин владеет временем жизни.
//
// Типичный паттерн: статическая переменная внутри handler_init() (Meyers singleton).

typedef struct {
    // ── Идентификация ─────────────────────────────────────────────────────────

    // Имя хендлера — ключ для find_handler_by_name().
    // Должно быть уникальным среди загруженных плагинов.
    // Указывает на статическую строку внутри плагина (не копируется).
    const char* name;

    // ── Коллбэки ──────────────────────────────────────────────────────────────

    // Вызывается когда ConnectionManager получил полный пакет из ESTABLISHED соединения.
    // payload уже расшифрован (TODO после реализации шифрования).
    void (*handle_message)(void*              user_data,
                           const header_t*    header,
                           const endpoint_t*  endpoint,
                           const void*        payload,
                           size_t             payload_size);

    // Вызывается когда состояние соединения изменилось.
    // uri — идентификатор соединения ("ip:port" или "gn://pubkey").
    void (*handle_conn_state)(void*         user_data,
                              const char*   uri,
                              conn_state_t  state);

    // Вызывается ядром перед dlclose() плагина.
    // Должен освободить все ресурсы хендлера.
    void (*shutdown)(void* user_data);

    // ── Фильтрация по типам сообщений ─────────────────────────────────────────

    // Массив MSG_TYPE_* которые интересуют этот хендлер.
    // PluginManager / ConnectionManager не доставляют пакеты с другими типами.
    // Ядро не освобождает этот массив — он принадлежит плагину.
    const uint32_t* supported_types;
    size_t          num_supported_types;

    // ── Пользовательские данные ───────────────────────────────────────────────

    void* user_data;  // обычно this (указатель на C++ объект хендлера)

} handler_t;

// Сигнатура функции инициализации (HANDLER_PLUGIN генерирует её автоматически)
typedef int (*handler_init_t)(host_api_t* api, handler_t** out_handler);

#ifdef __cplusplus
}
#endif
