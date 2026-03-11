#ifndef GOODNET_H
#define GOODNET_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Непрозрачный дескриптор ядра
typedef struct gn_core_t gn_core_t;

// Настройка (можно расширять, сохраняя бинарную совместимость)
typedef struct {
    const char* config_dir;
    const char* log_level;
    uint16_t    listen_port;
} gn_config_t;

// ── Lifecycle ─────────────────────────────────────────────────────────────
gn_core_t* gn_core_create(gn_config_t* cfg);
void       gn_core_destroy(gn_core_t* core);
void       gn_core_run(gn_core_t* core);
/**
 * @brief Запускает IO-цикл в фоновых потоках.
 * @param core Указатель на объект ядра.
 * @param threads Количество потоков (0 для автоопределения).
 */
void gn_core_run_async(gn_core_t* core, int threads);
void       gn_core_stop(gn_core_t* core);

// ── Network ───────────────────────────────────────────────────────────────
void gn_core_send(gn_core_t* core, const char* uri, uint32_t type, const void* data, size_t len);

// ── Identity ──────────────────────────────────────────────────────────────
// Копирует hex-строку в buffer. Возвращает длину.
size_t gn_core_get_user_pubkey(gn_core_t* core, char* buffer, size_t max_len);

// ── Subscriptions ─────────────────────────────────────────────────────────
// Callback-функция для других языков
typedef int (*gn_handler_t)(uint32_t type, const void* data, size_t len, void* user_data);

void gn_core_subscribe(gn_core_t* core, uint32_t type, gn_handler_t callback, void* user_data);

#ifdef __cplusplus
}
#endif

#endif // GOODNET_H