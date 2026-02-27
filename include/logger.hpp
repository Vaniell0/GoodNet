// logger.hpp
#pragma once

#include <fmt/core.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <memory>
#include <string>
#include <string_view>
#include <typeinfo>

class Logger {
public:
    // Статические переменные для конфигурации (дефолты)
    static std::string log_level;           // "trace", "debug", "info", "warn", "error", "critical", "off"
    static std::string log_file;            // Путь к файлу логов
    static size_t max_size;                 // Макс размер файла перед ротацией (байты)
    static int max_files;                   // Кол-во хранимых ротаций
    static std::string project_root;        // Корень проекта для относительных путей ("" = только basename)
    static bool strip_extension;            // Убирать расширение файла (.cpp, .h и т.д.)
    static int source_detail_mode;          // 0=авто (по уровню), 1=макс, 2=средний, 3=минимум
    static std::string file_pattern;        // Паттерн для файлового лога
    static std::string console_pattern;     // Паттерн для консоли

    static void shutdown();

    // Методы логирования
    template<typename... Args>
    static void trace(std::string_view file, int line, fmt::format_string<Args...> fmt, Args&&... args) {
        ensure_initialized();
        if (logger_) {
            logger_->log(spdlog::source_loc{file.data(), line, ""},
                         spdlog::level::trace, fmt, std::forward<Args>(args)...);
        }
    }

    template<typename... Args>
    static void debug(std::string_view file, int line, fmt::format_string<Args...> fmt, Args&&... args) {
        ensure_initialized();
        if (logger_) {
            logger_->log(spdlog::source_loc{file.data(), line, ""},
                         spdlog::level::debug, fmt, std::forward<Args>(args)...);
        }
    }

    template<typename... Args>
    static void info(std::string_view file, int line, fmt::format_string<Args...> fmt, Args&&... args) {
        ensure_initialized();
        if (logger_) {
            logger_->log(spdlog::source_loc{file.data(), line, ""},
                         spdlog::level::info, fmt, std::forward<Args>(args)...);
        }
    }

    template<typename... Args>
    static void warn(std::string_view file, int line, fmt::format_string<Args...> fmt, Args&&... args) {
        ensure_initialized();
        if (logger_) {
            logger_->log(spdlog::source_loc{file.data(), line, ""},
                         spdlog::level::warn, fmt, std::forward<Args>(args)...);
        }
    }

    template<typename... Args>
    static void error(std::string_view file, int line, fmt::format_string<Args...> fmt, Args&&... args) {
        ensure_initialized();
        if (logger_) {
            logger_->log(spdlog::source_loc{file.data(), line, ""},
                         spdlog::level::err, fmt, std::forward<Args>(args)...);
        }
    }

    template<typename... Args>
    static void critical(std::string_view file, int line, fmt::format_string<Args...> fmt, Args&&... args) {
        ensure_initialized();
        if (logger_) {
            logger_->log(spdlog::source_loc{file.data(), line, ""},
                         spdlog::level::critical, fmt, std::forward<Args>(args)...);
        }
    }

    static std::shared_ptr<spdlog::logger> get() {
        ensure_initialized();
        return logger_;
    }

private:
    static std::shared_ptr<spdlog::logger> logger_;
    static void ensure_initialized();
    static void init_internal();
};

// ScopedLogger — только в debug-сборке
#ifndef NDEBUG
class ScopedLogger {
public:
    ScopedLogger(const char* function_name, std::string_view file, int line, const char* level = "TRACE")
        : function_name_(function_name), file_(file), line_(line), level_(level) {
        if (level_ == "TRACE") {
            Logger::trace(file_, line_, ">>> Entering {}", function_name_);
        } else if (level_ == "DEBUG") {
            Logger::debug(file_, line_, ">>> Entering {}", function_name_);
        }
    }

    ~ScopedLogger() {
        if (level_ == "TRACE") {
            Logger::trace(file_, line_, "<<< Exiting {}", function_name_);
        } else if (level_ == "DEBUG") {
            Logger::debug(file_, line_, "<<< Exiting {}", function_name_);
        }
    }

private:
    const char* function_name_;
    std::string_view file_;
    int line_;
    const char* level_;
};
#endif

// Макросы логирования

#ifdef NDEBUG
    // В релизной сборке trace и debug полностью выключаются
    #define LOG_TRACE(...)              ((void)0)
    #define LOG_DEBUG(...)              ((void)0)
    #define TRACE_VALUE(var)            ((void)0)
    #define DEBUG_VALUE(var)            ((void)0)
    #define TRACE_VALUE_DETAILED(var)   ((void)0)
    #define DEBUG_VALUE_DETAILED(var)   ((void)0)
    #define TRACE_POINTER(ptr)          ((void)0)
    #define DEBUG_POINTER(ptr)          ((void)0)
    #define SCOPED_TRACE()              ((void)0)
    #define SCOPED_DEBUG()              ((void)0)
#else
    // В debug-сборке — полные макросы
    #define LOG_TRACE(...)      Logger::trace(std::string_view(__FILE__), __LINE__, __VA_ARGS__)
    #define LOG_DEBUG(...)      Logger::debug(std::string_view(__FILE__), __LINE__, __VA_ARGS__)

    #define TRACE_VALUE(var)    LOG_TRACE(#var " = {}", (var))
    #define DEBUG_VALUE(var)    LOG_DEBUG(#var " = {}", (var))

    #define TRACE_VALUE_DETAILED(var) \
        LOG_TRACE("{} [type: {}, size: {} bytes] = {}", \
                  #var, typeid(var).name(), sizeof(var), (var))

    #define DEBUG_VALUE_DETAILED(var) \
        LOG_DEBUG("{} [type: {}, size: {} bytes] = {}", \
                  #var, typeid(var).name(), sizeof(var), (var))

    #define TRACE_POINTER(ptr)  LOG_TRACE("{} [address: {:p}, valid: {}]", \
                                       #ptr, static_cast<const void*>(ptr), ((ptr) != nullptr))

    #define DEBUG_POINTER(ptr)  LOG_DEBUG("{} [address: {:p}, valid: {}]", \
                                       #ptr, static_cast<const void*>(ptr), ((ptr) != nullptr))

    #define SCOPED_TRACE()      ScopedLogger scoped_logger_##__LINE__(__FUNCTION__, std::string_view(__FILE__), __LINE__, "TRACE")
    #define SCOPED_DEBUG()      ScopedLogger scoped_logger_##__LINE__(__FUNCTION__, std::string_view(__FILE__), __LINE__, "DEBUG")
#endif

// Уровни info и выше — всегда активны (даже в релизе)
#define LOG_INFO(...)       Logger::info(std::string_view(__FILE__), __LINE__, __VA_ARGS__)
#define LOG_WARN(...)       Logger::warn(std::string_view(__FILE__), __LINE__, __VA_ARGS__)
#define LOG_ERROR(...)      Logger::error(std::string_view(__FILE__), __LINE__, __VA_ARGS__)
#define LOG_CRITICAL(...)   Logger::critical(std::string_view(__FILE__), __LINE__, __VA_ARGS__)

#define INFO_VALUE(var)     LOG_INFO(#var " = {}", (var))
#define WARN_VALUE(var)     LOG_WARN(#var " = {}", (var))
#define ERROR_VALUE(var)    LOG_ERROR(#var " = {}", (var))
#define CRITICAL_VALUE(var) LOG_CRITICAL(#var " = {}", (var))

#define INFO_POINTER(ptr)   LOG_INFO("{} [address: {:p}, valid: {}]", \
                                     #ptr, static_cast<const void*>(ptr), ((ptr) != nullptr))
#define WARN_POINTER(ptr)   LOG_WARN("{} [address: {:p}, valid: {}]", \
                                     #ptr, static_cast<const void*>(ptr), ((ptr) != nullptr))
#define ERROR_POINTER(ptr)  LOG_ERROR("{} [address: {:p}, valid: {}]", \
                                     #ptr, static_cast<const void*>(ptr), ((ptr) != nullptr))
