#pragma once

#include <fmt/core.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <memory>
#include <string>

// Compile-time извлечение имени файла
template<size_t N>
constexpr std::string_view get_filename(const char (&path)[N]) {
    size_t pos = N - 1;
    while (pos > 0 && path[pos] != '/' && path[pos] != '\\') --pos;
    if (path[pos] == '/' || path[pos] == '\\') { ++pos; }
    return std::string_view(&path[pos], N - pos - 1);
}

class Logger {
public:
    static void initialize(const std::string& log_level = "info",
                          const std::string& log_file = "logs/goodnet.log",
                          size_t max_size = 10 * 1024 * 1024,
                          int max_files = 5);
    
    static void shutdown();
    
    // Исправленные методы - используем spdlog::logger::log() с source_loc
    template<typename... Args>
    static void trace(std::string_view file, int line, fmt::format_string<Args...> fmt, Args&&... args) {
        if (logger_) {
            logger_->log(spdlog::source_loc{file.data(), line, ""}, 
                        spdlog::level::trace, fmt, std::forward<Args>(args)...);
        }
    }
    
    template<typename... Args>
    static void debug(std::string_view file, int line, fmt::format_string<Args...> fmt, Args&&... args) {
        if (logger_) {
            logger_->log(spdlog::source_loc{file.data(), line, ""}, 
                        spdlog::level::debug, fmt, std::forward<Args>(args)...);
        }
    }
    
    template<typename... Args>
    static void info(std::string_view file, int line, fmt::format_string<Args...> fmt, Args&&... args) {
        if (logger_) {
            logger_->log(spdlog::source_loc{file.data(), line, ""}, 
                        spdlog::level::info, fmt, std::forward<Args>(args)...);
        }
    }
    
    template<typename... Args>
    static void warn(std::string_view file, int line, fmt::format_string<Args...> fmt, Args&&... args) {
        if (logger_) {
            logger_->log(spdlog::source_loc{file.data(), line, ""}, 
                        spdlog::level::warn, fmt, std::forward<Args>(args)...);
        }
    }
    
    template<typename... Args>
    static void error(std::string_view file, int line, fmt::format_string<Args...> fmt, Args&&... args) {
        if (logger_) {
            logger_->log(spdlog::source_loc{file.data(), line, ""}, 
                        spdlog::level::err, fmt, std::forward<Args>(args)...);
        }
    }
    
    template<typename... Args>
    static void critical(std::string_view file, int line, fmt::format_string<Args...> fmt, Args&&... args) {
        if (logger_) {
            logger_->log(spdlog::source_loc{file.data(), line, ""}, 
                        spdlog::level::critical, fmt, std::forward<Args>(args)...);
        }
    }
    
    static std::shared_ptr<spdlog::logger> get() { return logger_; }
    
private:
    static std::shared_ptr<spdlog::logger> logger_;
};

// Объявляем класс ScopedLogger для дебажной сборки
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
    std::string_view file_; int line_;
    const char* level_;
};
#endif

#ifdef NDEBUG
// В релизе: убираем TRACE и DEBUG полностью
#define LOG_TRACE(...)        ((void)0)
#define LOG_DEBUG(...)        ((void)0)
#define TRACE_VALUE(var)      ((void)0)
#define DEBUG_VALUE(var)      ((void)0)
#define TRACE_VALUE_DETAILED(var) ((void)0)
#define DEBUG_VALUE_DETAILED(var) ((void)0)
#define TRACE_POINTER(ptr)    ((void)0)
#define DEBUG_POINTER(ptr)    ((void)0)
#define SCOPED_TRACE()        ((void)0)
#define SCOPED_DEBUG()        ((void)0)
#else
// В дебаге: полные макросы
#define LOG_TRACE(...)    Logger::trace(get_filename(__FILE__), __LINE__, __VA_ARGS__)
#define LOG_DEBUG(...)    Logger::debug(get_filename(__FILE__), __LINE__, __VA_ARGS__)
#define TRACE_VALUE(var) LOG_TRACE(#var " = {}", var)
#define DEBUG_VALUE(var) LOG_DEBUG(#var " = {}", var)
#define TRACE_VALUE_DETAILED(var) \
    LOG_TRACE("{} [type: {}, size: {} bytes] = {}", \
              #var, typeid(var).name(), sizeof(var), var)
#define DEBUG_VALUE_DETAILED(var) \
    LOG_DEBUG("{} [type: {}, size: {} bytes] = {}", \
              #var, typeid(var).name(), sizeof(var), var)
#define TRACE_POINTER(ptr) LOG_TRACE("{} [address: {}, valid: {}]", \
                #ptr, static_cast<const void*>(ptr), (ptr != nullptr))
#define DEBUG_POINTER(ptr) LOG_DEBUG("{} [address: {}, valid: {}]", \
                #ptr, static_cast<const void*>(ptr), (ptr != nullptr))
#define SCOPED_TRACE() ScopedLogger scoped_logger_##__LINE__(__FUNCTION__, get_filename(__FILE__), __LINE__, "TRACE")
#define SCOPED_DEBUG() ScopedLogger scoped_logger_##__LINE__(__FUNCTION__, get_filename(__FILE__), __LINE__, "DEBUG")
#endif

// INFO, WARN, ERROR, CRITICAL всегда включены
#define LOG_INFO(...)     Logger::info(get_filename(__FILE__), __LINE__, __VA_ARGS__)
#define LOG_WARN(...)     Logger::warn(get_filename(__FILE__), __LINE__, __VA_ARGS__)
#define LOG_ERROR(...)    Logger::error(get_filename(__FILE__), __LINE__, __VA_ARGS__)
#define LOG_CRITICAL(...) Logger::critical(get_filename(__FILE__), __LINE__, __VA_ARGS__)

#define INFO_VALUE(var)  LOG_INFO(#var " = {}", var)
#define WARN_VALUE(var)  LOG_WARN(#var " = {}", var)
#define ERROR_VALUE(var) LOG_ERROR(#var " = {}", var)
#define INFO_POINTER(ptr)  LOG_INFO("{} [address: {}, valid: {}]", \
                #ptr, static_cast<const void*>(ptr), (ptr != nullptr))
