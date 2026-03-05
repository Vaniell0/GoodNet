#pragma once
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

// ─── host_api_t ───────────────────────────────────────────────────────────────
//
// Единственный канал общения плагин ↔ ядро.
// Заполняется через ConnectionManager::fill_host_api().
//
// ПРАВИЛО: каждый коллбэк получает ctx первым аргументом.
// Это позволяет иметь несколько экземпляров ядра в одном процессе.
//
// Плагин хранит указатель host_api_t* и вызывает коллбэки — никаких
// прямых зависимостей от классов ядра.

typedef struct host_api_t {

    // ── Коллбэки коннектора → ядро ───────────────────────────────────────────
    //
    // Плагин ОБЯЗАН вызывать эти функции при наступлении событий.

    // Новое входящее соединение. Возвращает conn_id — плагин хранит его
    // как ключ в своей таблице соединений.
    conn_id_t (*on_connect)(void* ctx, const endpoint_t* endpoint);

    // Получены сырые байты от пира. Ядро само собирает пакеты из фрагментов.
    void (*on_data)(void* ctx, conn_id_t id, const void* raw, size_t size);

    // Соединение разорвано (error_code == 0 → чистое закрытие).
    void (*on_disconnect)(void* ctx, conn_id_t id, int error_code);

    // ── Коллбэки хендлера → ядро ─────────────────────────────────────────────
    //
    // Хендлер вызывает send() чтобы отправить данные через ConnectionManager.

    // Отправить пакет по URI. ConnectionManager разрешит маршрут и найдёт
    // нужный коннектор по схеме.
    void (*send)(void* ctx, const char* uri, uint32_t msg_type,
                 const void* payload, size_t size);

    // ── Крипто ───────────────────────────────────────────────────────────────
    //
    // Плагин просит ядро подписать буфер device_seckey'ом.
    // Приватный ключ никогда не покидает ядро.

    // Ed25519(device_seckey, data[0..size-1]) → sig[64]
    // Возвращает 0 при успехе.
    int (*sign_with_device)(void* ctx,
                            const void* data, size_t size,
                            uint8_t sig[64]);

    // Проверить Ed25519 подпись.
    // Возвращает 0 если подпись верна.
    int (*verify_signature)(void* ctx,
                            const void* data, size_t size,
                            const uint8_t* pubkey,
                            const uint8_t* signature);

    // ── Служебные ────────────────────────────────────────────────────────────

    // Указатель на экземпляр spdlog::logger* для sync_plugin_context.
    void*         internal_logger;

    // Тип плагина — устанавливается PluginManager перед вызовом *_init().
    plugin_type_t plugin_type;

    // Непрозрачный контекст ядра. Передавать первым аргументом во все коллбэки.
    void* ctx;

} host_api_t;

#ifdef __cplusplus
}
#endif
