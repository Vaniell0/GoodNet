// Мок-хендлер для unit-тестов PluginManager и ConnectionManager.
// Компилируется как libmock_handler.so.
// Экспортирует handler_init() через GN_EXPORT (HANDLER_PLUGIN макрос).
#include <handler.hpp>   // sdk/cpp/handler.hpp → IHandler

class MockHandler : public gn::IHandler {
public:
    const char* get_plugin_name() const override {
        return "mock_handler";
    }

    void on_init() override {
        // Подписываемся на несколько типов чтобы тесты могли проверять фильтрацию
        set_supported_types({MSG_TYPE_SYSTEM, MSG_TYPE_CHAT, MSG_TYPE_FILE});
    }

    void handle_message(const header_t* header,
                        const endpoint_t* /*endpoint*/,
                        std::span<const uint8_t> payload) override {
        (void)header;
        (void)payload;
    }

    void on_shutdown() override {
        // Cleanup — ничего не надо для мока
    }
};

HANDLER_PLUGIN(MockHandler)
