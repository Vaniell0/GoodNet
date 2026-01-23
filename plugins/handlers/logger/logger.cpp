#include <handler.hpp>
#include <plugin.hpp>
#include <logger.hpp>

#include <fstream>
#include <filesystem>
#include <chrono>
#include <iomanip>

namespace fs = std::filesystem;

class MessageLogger : public gn::IHandler {
public:
    void on_init() override {
        // Создаем папку для сообщений при инициализации
        try {
            if (!fs::exists("./msgs")) {
                fs::create_directories("./msgs");
            }
            LOG_INFO("Message Logger Plugin initialized. Saving to ./msgs");
        } catch (const std::exception& e) {
            LOG_ERROR("Failed to create msgs directory: {}", e.what());
        }

        // Подписываемся на все типы сообщений (например, от 0 до 100)
        // Если твоя система подразумевает, что 0 - это wild-card, укажи его
        set_supported_types({0}); 
    }

    void handle_message(
        const header_t* header,
        const endpoint_t* endpoint,
        const void* payload,
        size_t payload_size
    ) override {
        // Формируем имя файла на основе времени и ID сообщения
        auto now = std::chrono::system_clock::now();
        auto in_time_t = std::chrono::system_clock::to_time_t(now);

        std::stringstream ss;
        ss << "./msgs/msg_" 
           << std::put_time(std::localtime(&in_time_t), "%Y%m%d_%H%M%S")
           << "_" << header->packet_id << ".bin";

        std::string filename = ss.str();

        // Записываем тело сообщения в файл
        std::ofstream outfile(filename, std::ios::binary);
        if (outfile.is_open()) {
            outfile.write(reinterpret_cast<const char*>(payload), payload_size);
            outfile.close();
            
            LOG_TRACE("Saved message {} (size: {}) to {}", 
                      header->packet_id, payload_size, filename);
        } else {
            LOG_ERROR("Could not open file for writing: {}", filename);
        }
    }

    void handle_connection_state(const char* uri, conn_state_t state) override {
        LOG_DEBUG("Connection state changed for {}: {}", uri, (int)state);
    }

    void shutdown() override {
        LOG_INFO("Message Logger Plugin shutting down");
    }
};

// Регистрация плагина через макрос
HANDLER_PLUGIN(MessageLogger)