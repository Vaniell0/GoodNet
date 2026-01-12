#include "logger.hpp"
#include <filesystem>
#include <iostream>
#include <source_location>

namespace fs = std::filesystem;

std::shared_ptr<spdlog::logger> Logger::logger_ = nullptr;

void Logger::initialize(const std::string& log_level,
                       const std::string& log_file,
                       size_t max_size,
                       int max_files) {
    if (logger_) return;
    
    try {
        // Создаем директорию для логов
        fs::path log_path = log_file;
        if (!log_path.parent_path().empty()) {
            fs::create_directories(log_path.parent_path());
        }
        
        // Создаем два силка: в файл и в консоль
        std::vector<spdlog::sink_ptr> sinks;
        
        // Файловый sink
        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            log_file, max_size, max_files
        );
        file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%s:%#] %v");
        sinks.push_back(file_sink);
        
        // Консольный sink
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_pattern("%^[%Y-%m-%d %H:%M:%S.%e] [%l] [%s:%#]%$ %v");
        sinks.push_back(console_sink);
        
        // Создаем логгер
        logger_ = std::make_shared<spdlog::logger>("goodnet", sinks.begin(), sinks.end());
        
        // Устанавливаем уровень логирования
        if (log_level == "trace") {
            logger_->set_level(spdlog::level::trace);
        } else if (log_level == "debug") {
            logger_->set_level(spdlog::level::debug);
        } else if (log_level == "info") {
            logger_->set_level(spdlog::level::info);
        } else if (log_level == "warn") {
            logger_->set_level(spdlog::level::warn);
        } else if (log_level == "error") {
            logger_->set_level(spdlog::level::err);
        } else if (log_level == "critical") {
            logger_->set_level(spdlog::level::critical);
        } else if (log_level == "off") {
            logger_->set_level(spdlog::level::off);
        } else {
            logger_->set_level(spdlog::level::info);
        }
        
        // Настройки flush
        logger_->flush_on(spdlog::level::warn);
        spdlog::flush_every(std::chrono::seconds(3));
        
        spdlog::register_logger(logger_);
        LOG_INFO("Logger initialized. Level: {}, File: {}", log_level, log_file);
        LOG_DEBUG("Log pattern: [YYYY-MM-DD HH:MM:SS.ms] [LEVEL] message");
        
    } catch (const spdlog::spdlog_ex& ex) {
        std::cerr << "Log initialization failed: " << ex.what() << std::endl;
        throw;
    }
}

void Logger::shutdown() {
    if (logger_) {
        LOG_INFO("Logger shutting down...");
        logger_->flush();
        spdlog::drop_all();
        logger_.reset();
    }
}
