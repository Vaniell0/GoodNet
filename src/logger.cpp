// logger.cpp
#include "logger.hpp"
#include <filesystem>
#include <iostream>
#include <spdlog/pattern_formatter.h>
#include <spdlog/details/log_msg.h>
#include <algorithm>
#include <string_view>

namespace fs = std::filesystem;

// Инициализация статических переменных
std::shared_ptr<spdlog::logger> Logger::logger_ = nullptr;
std::string Logger::log_level = "info";
std::string Logger::log_file = "logs/goodnet.log";
size_t Logger::max_size = 10 * 1024 * 1024;
int Logger::max_files = 5;
std::string Logger::project_root = "";
bool Logger::strip_extension = false;
int Logger::source_detail_mode = 0;  // 0 = авто
std::string Logger::file_pattern = "[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %S%v";
std::string Logger::console_pattern = "%^[%Y-%m-%d %H:%M:%S.%e] [%l] %S%$%v";

// Кастомный флаг %S с учётом source_detail_mode
class custom_source_flag : public spdlog::custom_flag_formatter {
public:
    void format(const spdlog::details::log_msg &log_msg, const std::tm &, spdlog::memory_buf_t &dest) override {
        if (log_msg.source.empty()) return;

        std::string_view full_path(log_msg.source.filename);
        std::string_view relative_path = full_path;

        // Обрезка пути
        if (!Logger::project_root.empty()) {
            std::string_view root_view(Logger::project_root);
            if (full_path.starts_with(root_view)) {
                relative_path = full_path.substr(root_view.size());
                if (!relative_path.empty() && (relative_path[0] == '/' || relative_path[0] == '\\')) {
                    relative_path = relative_path.substr(1);
                }
            }
        } else {
            size_t last_slash = relative_path.find_last_of("/\\");
            if (last_slash != std::string_view::npos) {
                relative_path = relative_path.substr(last_slash + 1);
            }
        }

        // Убрать расширение
        std::string_view filename = relative_path;
        if (Logger::strip_extension) {
            size_t last_dot = filename.find_last_of('.');
            if (last_dot != std::string_view::npos) {
                filename = filename.substr(0, last_dot);
            }
        }

        // Детализация по mode
        bool show_line = true;
        bool show_full_path = false;

        switch (Logger::source_detail_mode) {
            case 0:  // Авто: зависит от уровня
                if (log_msg.level == spdlog::level::trace || log_msg.level == spdlog::level::debug) {
                    show_full_path = true;  // относительный путь
                    show_line = true;
                } else {
                    show_full_path = false;  // только имя
                    show_line = false;
                }
                break;
            case 1:  // Макс: [относительный:line]
                show_full_path = true;
                show_line = true;
                break;
            case 2:  // Средне: [имя:line]
                show_full_path = false;
                show_line = true;
                break;
            case 3:  // Мин: [имя]
                show_full_path = false;
                show_line = false;
                break;
            default:
                show_line = false;  // Безопасный дефолт
        }

        if (show_full_path) {
            if (show_line) {
                fmt::format_to(std::back_inserter(dest), "[{}:{}] ", relative_path, log_msg.source.line);
            } else {
                fmt::format_to(std::back_inserter(dest), "[{}] ", relative_path);
            }
        } else {
            if (show_line) {
                fmt::format_to(std::back_inserter(dest), "[{}:{}] ", filename, log_msg.source.line);
            } else {
                fmt::format_to(std::back_inserter(dest), "[{}] ", filename);
            }
        }
    }

    std::unique_ptr<custom_flag_formatter> clone() const override {
        return std::make_unique<custom_source_flag>();
    }
};

void Logger::ensure_initialized() {
    if (!logger_) {
        init_internal();
    }
}

void Logger::init_internal() {
    try {
        fs::path log_path = log_file;
        if (!log_path.parent_path().empty()) {
            fs::create_directories(log_path.parent_path());
        }

        std::vector<spdlog::sink_ptr> sinks;

        // Файловый синк
        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(log_file, max_size, max_files);
        auto file_formatter = std::make_unique<spdlog::pattern_formatter>();
        file_formatter->add_flag<custom_source_flag>('S');
        file_sink->set_formatter(std::move(file_formatter));
        file_sink->set_pattern(file_pattern);
        sinks.push_back(file_sink);

        // Консольный синк
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        auto console_formatter = std::make_unique<spdlog::pattern_formatter>();
        console_formatter->add_flag<custom_source_flag>('S');
        console_sink->set_formatter(std::move(console_formatter));
        console_sink->set_pattern(console_pattern);
        sinks.push_back(console_sink);

        logger_ = std::make_shared<spdlog::logger>("goodnet", sinks.begin(), sinks.end());

        // Уровень
        if (log_level == "trace") logger_->set_level(spdlog::level::trace);
        else if (log_level == "debug") logger_->set_level(spdlog::level::debug);
        else if (log_level == "info") logger_->set_level(spdlog::level::info);
        else if (log_level == "warn") logger_->set_level(spdlog::level::warn);
        else if (log_level == "error") logger_->set_level(spdlog::level::err);
        else if (log_level == "critical") logger_->set_level(spdlog::level::critical);
        else if (log_level == "off") logger_->set_level(spdlog::level::off);
        else logger_->set_level(spdlog::level::info);

        logger_->flush_on(spdlog::level::warn);
        spdlog::flush_every(std::chrono::seconds(3));
        spdlog::register_logger(logger_);

        // Лог инициализации
        logger_->info("Logger initialized with level: {}, file: {}, root: {}, strip: {}, mode: {}", 
                      log_level, log_file, project_root, strip_extension, source_detail_mode);

    } catch (const spdlog::spdlog_ex& ex) {
        std::cerr << "Log init failed: " << ex.what() << std::endl;
        throw;
    }
}

void Logger::shutdown() {
    if (logger_) {
        logger_->info("Logger shutting down...");
        logger_->flush();
        spdlog::drop_all();
        logger_.reset();
    }
}