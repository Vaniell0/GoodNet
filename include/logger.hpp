#pragma once

#include <string>
#include <string_view>
#include <filesystem>
#include <memory>
#include <mutex>

#include <fmt/core.h>
#include "fmt_extensions.hpp"
#include <spdlog/common.h>  // level_enum, source_loc — лёгкий хедер без тяжёлых зависимостей

// spdlog::logger — forward declaration.
// Полный тип нужен только в logger.cpp.
// Это предотвращает транзитивный include spdlog/spdlog.h в каждый TU проекта.
namespace spdlog { class logger; }

// ─── LoggerDetail ─────────────────────────────────────────────────────────────
// Compile-time конвертация типов для безопасности на границах .so

namespace LoggerDetail {
    template<typename T>
    inline auto to_loggable(T&& arg) {
        using D = std::decay_t<T>;
        if constexpr (std::is_same_v<D, std::filesystem::path>)
            return arg.string();
        else if constexpr (std::is_same_v<D, std::string>)
            return std::string_view(arg);
        else
            return std::forward<T>(arg);
    }
}

// ─── Logger ───────────────────────────────────────────────────────────────────
//
// Singleton-based logger для GoodNet core и плагинов.
//
// Архитектура:
//   • Конфигурация — через статические переменные (log_level, log_file и т.д.)
//     Выставляются в main() до первого LOG_*, подхватываются при ленивой инициализации.
//
//   • Хранилище — Meyers Singleton в get_instance() (статик внутри функции).
//     Гарантирует детерминированный порядок разрушения:
//     static local инициализируется при первом вызове и уничтожается при выходе
//     из программы строго после всех статиков, созданных позже. При shutdown()
//     обнуляем ptr явно — деструктор при __do_global_dtors_aux видит nullptr →
//     _M_dispose не вызывается → нет SIGSEGV.
//
//   • Плагины (RTLD_LOCAL) — получают raw ptr из api->internal_logger,
//     оборачивают в shared_ptr<no-op deleter> через set_external_logger().
//     Ядро управляет временем жизни объекта.

class Logger {
public:
    // ── Конфигурация ─────────────────────────────────────────────────────────
    // Выставляй до первого LOG_* (до появления в логе строки "Logger initialized")
    static std::string log_level;        // "trace"|"debug"|"info"|"warn"|"error"|"critical"|"off"
    static std::string log_file;         // путь к файлу ротации
    static size_t      max_size;         // максимальный размер файла (байты)
    static int         max_files;        // количество ротаций
    static std::string project_root;     // обрезать путь до относительного; "" = basename
    static bool        strip_extension;  // убирать .cpp/.h из имени файла в логе
    static int         source_detail_mode; // 0=авто 1=путь+строка 2=файл+строка 3=файл
    static std::string file_pattern;     // паттерн spdlog для файла
    static std::string console_pattern;  // паттерн spdlog для консоли

    static std::once_flag init_flag;

    // ── Жизненный цикл ────────────────────────────────────────────────────────

    // Получить экземпляр логгера (ленивая инициализация при первом вызове).
    // Потокобезопасно: std::call_once + Meyers local static.
    static std::shared_ptr<spdlog::logger> get();

    // Явный shutdown: flush, spdlog::drop_all(), обнуление shared_ptr.
    // ВЫЗЫВАТЬ ПОСЛЕ manager.unload_all() — плагины должны успеть завершиться.
    static void shutdown();

    // Установить внешний логгер (для плагинов через sync_plugin_context).
    // shared_ptr может иметь no-op deleter — ядро владеет объектом.
    static void set_external_logger(std::shared_ptr<spdlog::logger> ext);

    // ── Форматированное логирование ───────────────────────────────────────────

    template<spdlog::level::level_enum Lvl, typename... Args>
    static void log_fmt(std::string_view file, int line,
                        fmt::format_string<Args...> fmt_str, Args&&... args)
    {
        if (!should_log(Lvl)) return;
        try {
            // Конвертируем fs::path и std::string на входе
            auto targs = std::make_tuple(LoggerDetail::to_loggable(std::forward<Args>(args))...);
            auto fargs = std::apply([](auto&... a) { return fmt::make_format_args(a...); }, targs);

            fmt::memory_buffer buf;
            fmt::vformat_to(std::back_inserter(buf), fmt_str.get(), fargs);
            log_raw({file.data(), static_cast<int>(line), ""},
                    Lvl,
                    std::string_view(buf.data(), buf.size()));
        } catch (const std::exception& e) {
            fmt::print(stderr, "\033[31m[Logger Error]\033[0m {} | {}:{}\n",
                       e.what(), file, line);
        }
    }

    template<typename... Args>
    static void trace(std::string_view f, int l, fmt::format_string<Args...> s, Args&&... a)
    { log_fmt<spdlog::level::trace>(f, l, s, std::forward<Args>(a)...); }

    template<typename... Args>
    static void debug(std::string_view f, int l, fmt::format_string<Args...> s, Args&&... a)
    { log_fmt<spdlog::level::debug>(f, l, s, std::forward<Args>(a)...); }

    template<typename... Args>
    static void info(std::string_view f, int l, fmt::format_string<Args...> s, Args&&... a)
    { log_fmt<spdlog::level::info>(f, l, s, std::forward<Args>(a)...); }

    template<typename... Args>
    static void warn(std::string_view f, int l, fmt::format_string<Args...> s, Args&&... a)
    { log_fmt<spdlog::level::warn>(f, l, s, std::forward<Args>(a)...); }

    template<typename... Args>
    static void error(std::string_view f, int l, fmt::format_string<Args...> s, Args&&... a)
    { log_fmt<spdlog::level::err>(f, l, s, std::forward<Args>(a)...); }

    template<typename... Args>
    static void critical(std::string_view f, int l, fmt::format_string<Args...> s, Args&&... a)
    { log_fmt<spdlog::level::critical>(f, l, s, std::forward<Args>(a)...); }

private:
    // ── Meyers Singleton storage ──────────────────────────────────────────────
    // Хранилище вынесено в .cpp. Это предотвращает нахождение тяжёлого
    // spdlog/spdlog.h (нужен для работы с экземпляром) в публичном хедере.
    static std::shared_ptr<spdlog::logger>& get_instance() noexcept;

    static void ensure_initialized();
    static void init_internal();
    static bool should_log(spdlog::level::level_enum lvl);
    static void log_raw(spdlog::source_loc loc, spdlog::level::level_enum lvl, std::string_view msg);
};

// ─── ScopedLogger (debug only) ────────────────────────────────────────────────

#ifndef NDEBUG
class ScopedLogger {
public:
    ScopedLogger(const char* fn, std::string_view file, int line, const char* lvl = "TRACE")
        : fn_(fn), file_(file), line_(line), lvl_(lvl)
    {
        if (lvl_ == "TRACE")      Logger::trace(file_, line_, ">>> {}", fn_);
        else if (lvl_ == "DEBUG") Logger::debug(file_, line_, ">>> {}", fn_);
    }
    ~ScopedLogger() {
        if (lvl_ == "TRACE")      Logger::trace(file_, line_, "<<< {}", fn_);
        else if (lvl_ == "DEBUG") Logger::debug(file_, line_, "<<< {}", fn_);
    }
private:
    std::string_view fn_, file_;
    int              line_;
    std::string_view lvl_;
};
#endif

// ─── Макросы ──────────────────────────────────────────────────────────────────

#ifdef NDEBUG
    #define LOG_TRACE(...)            ((void)0)
    #define LOG_DEBUG(...)            ((void)0)
    #define TRACE_VALUE(var)          ((void)0)
    #define DEBUG_VALUE(var)          ((void)0)
    #define TRACE_VALUE_DETAILED(var) ((void)0)
    #define DEBUG_VALUE_DETAILED(var) ((void)0)
    #define TRACE_POINTER(ptr)        ((void)0)
    #define DEBUG_POINTER(ptr)        ((void)0)
    #define LOG_SCOPED_TRACE()        ((void)0)
    #define LOG_SCOPED_DEBUG()        ((void)0)
#else
    #define LOG_TRACE(...) Logger::trace(std::string_view(__FILE__), __LINE__, __VA_ARGS__)
    #define LOG_DEBUG(...) Logger::debug(std::string_view(__FILE__), __LINE__, __VA_ARGS__)

    #define TRACE_VALUE(var) LOG_TRACE(#var " = {}", (var))
    #define DEBUG_VALUE(var) LOG_DEBUG(#var " = {}", (var))

    #define TRACE_VALUE_DETAILED(var) \
        LOG_TRACE("{} [type:{} size:{}] = {}", #var, typeid(var).name(), sizeof(var), (var))
    #define DEBUG_VALUE_DETAILED(var) \
        LOG_DEBUG("{} [type:{} size:{}] = {}", #var, typeid(var).name(), sizeof(var), (var))

    #define TRACE_POINTER(ptr) \
        LOG_TRACE("{} [{:p} valid:{}]", #ptr, static_cast<const void*>(ptr), (ptr) != nullptr)
    #define DEBUG_POINTER(ptr) \
        LOG_DEBUG("{} [{:p} valid:{}]", #ptr, static_cast<const void*>(ptr), (ptr) != nullptr)

    #define LOG_SCOPED_TRACE() \
        ScopedLogger scoped_logger_##__LINE__(__FUNCTION__, std::string_view(__FILE__), __LINE__, "TRACE")
    #define LOG_SCOPED_DEBUG() \
        ScopedLogger scoped_logger_##__LINE__(__FUNCTION__, std::string_view(__FILE__), __LINE__, "DEBUG")
#endif

#define LOG_INFO(...)     Logger::info    (std::string_view(__FILE__), __LINE__, __VA_ARGS__)
#define LOG_WARN(...)     Logger::warn    (std::string_view(__FILE__), __LINE__, __VA_ARGS__)
#define LOG_ERROR(...)    Logger::error   (std::string_view(__FILE__), __LINE__, __VA_ARGS__)
#define LOG_CRITICAL(...) Logger::critical(std::string_view(__FILE__), __LINE__, __VA_ARGS__)

#define INFO_VALUE(var)     LOG_INFO (#var " = {}", (var))
#define WARN_VALUE(var)     LOG_WARN (#var " = {}", (var))
#define ERROR_VALUE(var)    LOG_ERROR(#var " = {}", (var))

#define INFO_POINTER(ptr)  LOG_INFO  ("{} [{:p} valid:{}]", #ptr, static_cast<const void*>(ptr), (ptr) != nullptr)
#define WARN_POINTER(ptr)  LOG_WARN  ("{} [{:p} valid:{}]", #ptr, static_cast<const void*>(ptr), (ptr) != nullptr)
#define ERROR_POINTER(ptr) LOG_ERROR ("{} [{:p} valid:{}]", #ptr, static_cast<const void*>(ptr), (ptr) != nullptr)
