/// @file tests/mock_handler.cpp
/// @brief Mock handler plugin for PluginManager and ConnectionManager unit tests.
/// Compiled as libmock_handler.so.
/// Exports handler_init() via GN_EXPORT (HANDLER_PLUGIN macro).
#include <handler.hpp>   // sdk/cpp/handler.hpp -> IHandler

class MockHandler : public gn::IHandler {
public:
    const char* get_plugin_name() const override {
        return "mock_handler";
    }

    void on_init() override {
        // Subscribe to several types so tests can verify filtering.
        set_supported_types({MSG_TYPE_SYSTEM, MSG_TYPE_CHAT, MSG_TYPE_FILE});
    }

    void handle_message(const header_t* header,
                        const endpoint_t* /*endpoint*/,
                        std::span<const uint8_t> payload) override {
        (void)header;
        (void)payload;
    }

    void on_shutdown() override {}
};

HANDLER_PLUGIN(MockHandler)
