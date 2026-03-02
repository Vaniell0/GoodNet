#pragma once

#include <fmt/core.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <memory>
#include <string>
#include <tuple>
#include <filesystem>
#include <string_view>
#include <typeinfo>

namespace LoggerDetail {
    // Compile-time преобразование типов для безопасности на границах .so
    template<typename T>
    inline auto to_loggable(T&& arg) {
        using Decayed = std::decay_t<T>;
        if constexpr (std::is_same_v<Decayed, std::filesystem::path>) {
            return arg.string(); 
        } else if constexpr (std::is_same_v<Decayed, std::string>) {
            return std::string_view(arg);
        } else {
            return std::forward<T>(arg);
        }
    }
}

class Logger {
public:
    // Статические переменные для конфигурации (дефолты)
    static std::string log_level;       // "trace", "debug", "info", "warn", "error", "critical", "off"
    static std::string log_file;        // Путь к файлу логов
    static size_t max_size;             // Макс размер файла перед ротацией (байты)
    static int max_files;               // Кол-во хранимых ротаций
    static std::string project_root;    // Корень проекта для относительных путей ("" = только basename)
    static bool strip_extension;        // Убирать расширение файла (.cpp, .h и т.д.)
    static int source_detail_mode;      // 0=авто (по уровню), 1=макс, 2=средний, 3=минимум
    static std::string file_pattern;    // Паттерн для файлового лога
    static std::string console_pattern; // Паттерн для консоли

    static std::once_flag init_flag;

    static void shutdown();

    static void log_raw(spdlog::source_loc loc, spdlog::level::level_enum lvl, std::string_view msg);

    // Главный шаблон форматирования
    template<spdlog::level::level_enum Lvl, typename... Args>
    static void log_fmt(std::string_view file, int line, fmt::format_string<Args...> fmt_str, Args&&... args) {
        if (!logger_) {
            ensure_initialized();
            if (!logger_) return;
        }
        
        if (!logger_->should_log(Lvl)) return;

        try {
            // 1. Сначала трансформируем все аргументы и сохраняем их в кортеж.
            // Это удерживает временные объекты (string из path и т.д.) живыми.
            auto transformed_args = std::make_tuple(LoggerDetail::to_loggable(std::forward<Args>(args))...);
            
            // 2. Распаковываем кортеж в аргументы форматирования
            auto loggable_args = std::apply([](auto&... unpacked) {
                return fmt::make_format_args(unpacked...);
            }, transformed_args);
            
            fmt::memory_buffer buf;
            fmt::vformat_to(std::back_inserter(buf), fmt_str.get(), loggable_args);
            
            log_raw({file.data(), line, ""}, Lvl, std::string_view(buf.data(), buf.size()));
        } catch (const std::exception& e) {
            fmt::print(stderr, "\033[31m[Logger Error]\033[0m {} | {}:{}\n", e.what(), file, line);
        }
    }

    // Методы для макросов
    template<typename... Args> static inline void trace(std::string_view f, int l, fmt::format_string<Args...> s, Args&&... a) 
    { log_fmt<spdlog::level::trace>(f, l, s, std::forward<Args>(a)...); }
    
    template<typename... Args> static inline void debug(std::string_view f, int l, fmt::format_string<Args...> s, Args&&... a) 
    { log_fmt<spdlog::level::debug>(f, l, s, std::forward<Args>(a)...); }
    
    template<typename... Args> static inline void info(std::string_view f, int l, fmt::format_string<Args...> s, Args&&... a) 
    { log_fmt<spdlog::level::info>(f, l, s, std::forward<Args>(a)...); }
    
    template<typename... Args> static inline void warn(std::string_view f, int l, fmt::format_string<Args...> s, Args&&... a) 
    { log_fmt<spdlog::level::warn>(f, l, s, std::forward<Args>(a)...); }
    
    template<typename... Args> static inline void error(std::string_view f, int l, fmt::format_string<Args...> s, Args&&... a) 
    { log_fmt<spdlog::level::err>(f, l, s, std::forward<Args>(a)...); }

    static std::shared_ptr<spdlog::logger> get() {
        ensure_initialized();
        return logger_;
    }

    static void set_external_logger(std::shared_ptr<spdlog::logger> ext) {
        logger_ = std::move(ext);
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
    std::string_view function_name_;
    std::string_view file_;
    int line_;
    std::string_view level_;
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
