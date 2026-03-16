/// @file plugins/connectors/template/template_connector.cpp
/// @brief Шаблон плагина-коннектора (connector).
///
/// Скелет транспортного коннектора. Реализуйте do_connect, do_listen,
/// do_send, do_close для вашего транспорта (UDP, WebSocket, QUIC и т.д.).
///
/// Сборка: cmake --build build --target template_connector
/// Результат: libtemplate_connector.so

#include <connector.hpp>
#include <logger.hpp>

#include <cstring>
#include <memory>
#include <span>
#include <string>

using namespace gn;

class TemplateConnector : public IConnector {
public:
    // Схема URI — используется для маршрутизации: "myproto://host:port"
    // TODO: Замените на свою схему
    std::string get_scheme() const override { return "myproto"; }
    std::string get_name()   const override { return "Template Connector"; }

    // ── Жизненный цикл ───────────────────────────────────────────────────────

    // Вызывается при загрузке плагина. Инициализируйте I/O потоки,
    // сокеты и другие ресурсы.
    void on_init() override {
        // TODO: Создайте io_context, рабочие потоки
        // TODO: Прочитайте конфигурацию: config_get("myproto.option")
        LOG_INFO("[TemplateConnector] Инициализирован");
    }

    // Вызывается при выгрузке. Закройте все соединения и освободите ресурсы.
    void on_shutdown() override {
        // TODO: Остановите io_context, дождитесь завершения потоков
        LOG_INFO("[TemplateConnector] Завершение работы");
    }

    // ── Исходящее соединение ─────────────────────────────────────────────────

    // Подключение к удалённому узлу. URI формат: "myproto://host:port"
    // Вернуть 0 при успехе (асинхронно), -1 при ошибке.
    int do_connect(const char* uri) override {
        std::string target(uri);
        // TODO: Распарсить URI, создать соединение
        // После установки вызвать:
        //   endpoint_t ep{};
        //   // ... заполнить ep.address, ep.port, ep.flags
        //   conn_id_t id = notify_connect(&ep);
        //   // ... сохранить id для дальнейших операций

        LOG_INFO("[TemplateConnector] do_connect: {}", target);
        return -1;  // TODO: Реализовать
    }

    // ── Прослушивание (сервер) ───────────────────────────────────────────────

    // Начать принимать входящие соединения на host:port.
    // Вернуть 0 при успехе, -1 при ошибке.
    int do_listen(const char* host, uint16_t port) override {
        // TODO: Создать серверный сокет, начать accept loop
        // При новом соединении вызвать:
        //   endpoint_t ep{};
        //   // ... заполнить ep
        //   conn_id_t id = notify_connect(&ep);

        LOG_INFO("[TemplateConnector] do_listen: {}:{}", host, port);
        return -1;  // TODO: Реализовать
    }

    // ── Отправка данных ──────────────────────────────────────────────────────

    // Отправить данные на указанное соединение.
    // data — полный фрейм (header + encrypted payload).
    int do_send(conn_id_t id, std::span<const uint8_t> data) override {
        // TODO: Найти соединение по id, отправить data
        // При ошибке отправки вызвать: notify_disconnect(id, errno);

        (void)id; (void)data;
        return -1;  // TODO: Реализовать
    }

    // Scatter-gather отправка (опционально, по умолчанию вызывает do_send в цикле).
    // Переопределите для оптимизации (writev, sendmmsg).
    // int do_send_gather(conn_id_t id, const struct iovec* iov, int n) override {
    //     // TODO: Реализовать writev-стиль отправки
    // }

    // ── Закрытие соединения ──────────────────────────────────────────────────

    // hard=true  — немедленное закрытие (cancel I/O)
    // hard=false — graceful close (дождаться flush)
    void do_close(conn_id_t id, bool hard) override {
        // TODO: Закрыть соединение
        // ВАЖНО: Всегда вызывать notify_disconnect(id, 0) после закрытия!
        (void)hard;
        notify_disconnect(id, 0);
    }
};

// Макрос регистрации плагина — обязателен.
CONNECTOR_PLUGIN(TemplateConnector)
