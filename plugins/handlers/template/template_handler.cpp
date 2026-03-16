/// @file plugins/handlers/template/template_handler.cpp
/// @brief Шаблон плагина-обработчика (handler).
///
/// Минимальный рабочий handler, который обрабатывает сообщения определённых
/// типов и отправляет эхо-ответ. Используйте как основу для своих плагинов.
///
/// Сборка: cmake --build build --target template_handler
/// Результат: libtemplate_handler.so

#include <handler.hpp>
#include <logger.hpp>

#include <cstring>
#include <span>
#include <string>

using namespace gn;

// ── Определение типов сообщений ──────────────────────────────────────────────
// TODO: Замените на свои типы из sdk/types.h
static constexpr uint32_t MY_MSG_TYPE = MSG_TYPE_CHAT;

class TemplateHandler : public IHandler {
public:
    // Имя плагина — должно быть уникальным в системе.
    const char* get_plugin_name() const override {
        return "template_handler";
    }

    // Вызывается при загрузке плагина. Здесь можно читать конфигурацию,
    // инициализировать ресурсы и подписываться на типы сообщений.
    void on_init() override {
        // Подписка на конкретные типы сообщений.
        // Если не вызвать set_supported_types(), handler получит ВСЕ сообщения (wildcard).
        set_supported_types({MY_MSG_TYPE});

        // Чтение конфигурации из JSON-файла (ключи с точечной нотацией).
        auto greeting = config_get("template.greeting");
        if (!greeting.empty())
            LOG_INFO("[TemplateHandler] Приветствие: {}", greeting);

        LOG_INFO("[TemplateHandler] Инициализирован");
    }

    // Вызывается при выгрузке плагина. Освободите ресурсы здесь.
    void on_shutdown() override {
        LOG_INFO("[TemplateHandler] Завершение работы");
    }

    // Основной обработчик сообщений. Вызывается для каждого пакета
    // подходящего типа.
    //
    // header   — заголовок пакета (44 байта, тип, packet_id, timestamp и т.д.)
    // endpoint — информация об отправителе (адрес, порт, pubkey, conn_id)
    // payload  — расшифрованные данные пакета
    void handle_message(const header_t*           header,
                        const endpoint_t*         endpoint,
                        std::span<const uint8_t>  payload) override {
        // TODO: Реализуйте свою логику обработки

        // Пример: логирование входящего сообщения
        LOG_INFO("[TemplateHandler] Получен пакет type={} size={} от {}:{}",
                 header->payload_type, payload.size(),
                 endpoint->address, endpoint->port);

        // Пример: эхо-ответ отправителю
        if (!payload.empty()) {
            send_response(endpoint->peer_id, header->payload_type, payload);
        }
    }

    // Результат диспетчеризации после handle_message().
    // PROPAGATION_CONTINUE — передать следующему handler в цепочке
    // PROPAGATION_CONSUMED — обработано, остановить цепочку
    // PROPAGATION_REJECT   — отклонить пакет
    propagation_t on_result(const header_t* /*hdr*/, uint32_t /*type*/) override {
        return PROPAGATION_CONSUMED;
    }

    // Уведомление об изменении состояния соединения.
    // uri   — pubkey_hex пира
    // state — STATE_ESTABLISHED, STATE_CLOSED, и т.д.
    void on_conn_state(const char* uri, conn_state_t state) override {
        if (state == STATE_ESTABLISHED)
            LOG_INFO("[TemplateHandler] Пир подключён: {:.12}...", uri);
        else if (state == STATE_CLOSED)
            LOG_INFO("[TemplateHandler] Пир отключён: {:.12}...", uri);
    }
};

// Макрос регистрации плагина — обязателен.
HANDLER_PLUGIN(TemplateHandler)
