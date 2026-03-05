#include <handler.hpp>
#include <plugin.hpp>
#include <logger.hpp>

#include <fstream>
#include <filesystem>
#include <mutex>
#include <atomic>

namespace fs = std::filesystem;

class RawBundleLogger : public gn::IHandler {
public:
    const char* get_plugin_name() const override { return "BundleLogger"; }

    void on_init() override {
        if (!fs::exists("./msgs")) fs::create_directories("./msgs");
        open_new_bundle();
        set_supported_types({0}); 
        LOG_INFO("Bundle Logger initialized. Max file size: 100MB");
    }

    void handle_message(const header_t* header, const endpoint_t* endpoint, 
                        const void* payload, size_t payload_size) override {
        std::lock_guard<std::mutex> lock(file_mtx_);

        // Проверяем размер перед записью (100 MB = 104,857,600 байт)
        if (current_size_ > 100 * 1024 * 1024) {
            open_new_bundle();
        }

        if (out_file_.is_open()) {
            // Пишем метаданные, чтобы потом можно было распарсить
            out_file_.write(reinterpret_cast<const char*>(header), sizeof(header_t));
            out_file_.write(reinterpret_cast<const char*>(payload), payload_size);
            
            current_size_ += (sizeof(header_t) + payload_size);
            total_count_++;
        }
    }

    void on_shutdown() override {
        if (out_file_.is_open()) out_file_.close();
        LOG_INFO("Bundle Logger stopped. Total messages captured: {}", total_count_.load());
    }

private:
    void open_new_bundle() {
        if (out_file_.is_open()) out_file_.close();
        
        // Используем микросекунды для действительно уникальных имен
        auto now = std::chrono::high_resolution_clock::now();
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
        
        std::string filename = fmt::format("./msgs/bundle_{}.bin", us);
        
        // ios::app гарантирует, что мы пишем в конец и не затираем данные
        out_file_.open(filename, std::ios::binary | std::ios::out | std::ios::app);
        current_size_ = 0;
        
        LOG_INFO("Opened new bundle file: {}", filename);
    }

    std::ofstream out_file_;
    std::mutex file_mtx_;
    size_t current_size_ = 0;
    std::atomic<uint64_t> total_count_{0};
};

HANDLER_PLUGIN(RawBundleLogger)
