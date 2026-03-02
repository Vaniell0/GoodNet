#include <handler.hpp>
#include <plugin.hpp>
#include <logger.hpp>

#include <fstream>
#include <filesystem>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace fs = std::filesystem;

class MessageLogger : public gn::IHandler {
public:
    // Добавили const и override — теперь сигнатуры совпадают идеально
    const char* get_plugin_name() const override { 
        return "RawLogger"; 
    }

    void on_init() override {
        try {
            if (!fs::exists("./msgs")) {
                fs::create_directories("./msgs");
            }
            LOG_INFO("Message Logger Plugin initialized. Saving to ./msgs");
        } catch (const std::exception& e) {
            LOG_ERROR("Failed to create msgs directory: {}", e.what());
        }

        // Подписываемся на все сообщения (0 обычно wildcard в таких системах)
        set_supported_types({0}); 
    }

    void handle_message(const header_t* header, const endpoint_t* endpoint, 
                    const void* payload, size_t payload_size) override {
    
        auto now = std::chrono::system_clock::now();
        auto in_time_t = std::chrono::system_clock::to_time_t(now);
        struct tm buf;
        localtime_r(&in_time_t, &buf); // Потокобезопасно на Linux

        // Используем fmt для формирования имени файла — это быстрее в разы
        std::string filename = fmt::format("./msgs/msg_{:%Y%m%d_%H%M%S}_{}.bin", 
                                        buf, header->packet_id);

        std::ofstream outfile(filename, std::ios::binary);
        if (outfile) {
            outfile.write(reinterpret_cast<const char*>(payload), payload_size);
            // LOG_TRACE тут будет летать благодаря нашей оптимизации
            LOG_TRACE("Saved msg {} to {}", header->packet_id, filename);
        }
    }

    void handle_connection_state(const char* uri, conn_state_t state) override {
        LOG_DEBUG("Connection state changed for {}: {}", uri, (int)state);
    }

    // Исправлено: метод называется on_shutdown
    void on_shutdown() override {
        LOG_INFO("Message Logger Plugin shutting down");
    }
};

HANDLER_PLUGIN(MessageLogger)
