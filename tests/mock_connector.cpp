// Мок-коннектор для unit-тестов PluginManager.
// Схема: "mock". Имя: "MockConnector".
// Компилируется как libmock_connector.so.
// Экспортирует connector_init() через GN_EXPORT (CONNECTOR_PLUGIN макрос).
//
// do_send_to(), do_listen() — no-op заглушки.
// do_connect() — всегда возвращает -1 (не поддерживается в моке).
// Используется только для тестирования загрузки и поиска коннектора по схеме.
#include <connector.hpp>  // sdk/cpp/connector.hpp → IConnector
#include <cstring>

class MockConnector : public gn::IConnector {
public:
    std::string get_scheme() const override { return "mock"; }
    std::string get_name()   const override { return "MockConnector"; }

    void on_init() override {
        // Ничего инициализировать не нужно для мока
    }

    // Подключение: в тестах не используется реальная сеть
    int do_connect(const char* /*uri*/) override {
        return -1;  // не поддерживается в моке
    }

    // Listen: no-op
    int do_listen(const char* /*host*/, uint16_t /*port*/) override {
        return 0;
    }

    // send_to: молча отбрасываем
    int do_send_to(conn_id_t /*id*/,
                   const void* /*data*/, size_t /*size*/) override {
        return 0;
    }

    // close: no-op
    void do_close(conn_id_t /*id*/) override {}

    void on_shutdown() override {
        // Ничего останавливать не нужно
    }
};

CONNECTOR_PLUGIN(MockConnector)
