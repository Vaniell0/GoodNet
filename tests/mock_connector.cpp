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

    void on_init() override {}

    int do_connect(const char* /*uri*/) override {
        return -1; 
    }

    int do_listen(const char* /*host*/, uint16_t /*port*/) override {
        return 0;
    }

    // ИСПРАВЛЕНО: do_send вместо do_send_to + std::span
    int do_send(conn_id_t /*id*/, std::span<const uint8_t> /*data*/) override {
        return 0;
    }

    // ИСПРАВЛЕНО: добавился bool hard
    void do_close(conn_id_t /*id*/, bool /*hard*/) override {}

    void on_shutdown() override {}
};

CONNECTOR_PLUGIN(MockConnector)