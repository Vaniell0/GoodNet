#include <handler.hpp>
#include <plugin.hpp>
#include <spdlog/spdlog.h>

using namespace gn;

class Logger : public IHandler {
public:
    void on_init() override {
        spdlog::info("[Logger] Plugin loaded and initialized");
        // Поддерживаем все типы сообщений (0 = wildcard)
        set_supported_types({0});
    }

    void handle_message(
        const header_t* header,
        const endpoint_t* endpoint,
        const void* payload,
        size_t payload_size
    ) override {
        const char* src = (endpoint && endpoint->address[0]) ? endpoint->address : "unknown";
        uint32_t msg_type = header ? header->payload_type : 0;

        spdlog::info("[Logger] ← {} | type={} | size={}", src, msg_type, payload_size);
    }

    void handle_connection_state(const char* uri, conn_state_t state) override {
        spdlog::info("[Logger] Connection '{}' → state {}", uri, static_cast<int>(state));
    }
};

HANDLER_PLUGIN(Logger);