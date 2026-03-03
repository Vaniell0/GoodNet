#include <handler.hpp>
#include <plugin.hpp>

/**
 * @brief Minimal handler used in unit tests.
 *
 * Subscribes to message types 1, 2, 3.
 * On type 1: echoes the payload back via send().
 */
class MockHandler : public gn::IHandler {
public:
    const char* get_plugin_name() const override { return "mock_handler"; }

    void on_init() override {
        set_supported_types({1, 2, 3});
    }

    void handle_message(const header_t* header, const endpoint_t*,
                        const void* payload, size_t size) override {
        if (header->payload_type == 1)
            send("test://uri", 1, payload, size);
    }
};

HANDLER_PLUGIN(MockHandler)
