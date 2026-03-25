#pragma once
/// @file include/logger.hpp
/// @brief GoodNet logging system — singleton wrapper over spdlog.
///
/// ## Architecture
/// Meyers-singleton spdlog::logger shared across the core and all .so plugins.
/// Two sinks: rotating file (always) + console color (Debug builds only).
///
/// ## Initialization
/// Call static setters (`set_log_level`, `set_log_file`, etc.) BEFORE the first
/// `LOG_*` macro invocation.  The logger is lazily initialized on first use
/// via `std::call_once`.  After init, setter calls have no effect.
///
/// ## Custom format flag %Q
/// Injects source location into log lines: `[file:line]` for debug/trace,
/// `[file]` for info+.  Controlled by `source_detail_mode`:
///   - 0: show path+line only for trace/debug (default)
///   - 1: always show path+line
///   - 2: always show line (no path)
///   - 3: never show line
///
/// ## Thread-safety
/// All public methods and macros are thread-safe.  The singleton is protected
/// by `std::call_once`; spdlog itself is internally synchronized.
///
/// ## Performance (Release builds)
/// `LOG_TRACE`, `LOG_DEBUG`, `LOG_SCOPE_*`, `TRACE_*`, `DEBUG_*` macros
/// compile to `((void)0)` when `NDEBUG` is defined — zero overhead.

#include <string>
#include <string_view>
#include <filesystem>
#include <memory>
#include <mutex>

#include <fmt/core.h>
#include <spdlog/common.h>

namespace spdlog { class logger; }

// ─── LoggerDetail ─────────────────────────────────────────────────────────────

/// @brief Compile-time type conversion for safe logging across .so boundaries.
///
/// Converts types that may have different ABI representations across shared
/// libraries into stable, view-based types:
///   - `std::filesystem::path` → `std::string`  (path::string() copy)
///   - `std::string`           → `std::string_view` (avoids SSO mismatch)
///   - Everything else         → forwarded as-is
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

class Logger {
public:
    /// @name Configuration (call BEFORE the first LOG_* macro)
    /// @{

    /// @brief Set global log level ("trace","debug","info","warn","error","critical","off").
    static void set_log_level       (std::string_view level);
    /// @brief Set rotating log file path. Empty = no file sink.
    static void set_log_file        (std::string_view path);
    /// @brief Max file size before rotation (default 10 MB).
    static void set_max_size        (size_t bytes);
    /// @brief Max rotated files kept (default 5).
    static void set_max_files       (int count);
    /// @brief Project root for relative source paths in %Q flag.
    static void set_project_root    (std::string_view root);
    /// @brief Strip file extension from source location in %Q flag.
    static void set_strip_extension (bool strip);
    /// @brief Source detail mode for %Q flag (0–3, see file header).
    static void set_source_detail_mode(int mode);
    /// @brief spdlog pattern for the file sink. Default includes %Q.
    static void set_file_pattern    (std::string_view pattern);
    /// @brief spdlog pattern for the console sink. Default includes %Q.
    static void set_console_pattern (std::string_view pattern);
    /// @}

    // ── Геттеры ──────────────────────────────────────────────────────────────

    static const std::string& get_log_level()          noexcept;
    static const std::string& get_log_file()           noexcept;
    static size_t             get_max_size()            noexcept;
    static int                get_max_files()           noexcept;
    static const std::string& get_project_root()       noexcept;
    static bool               get_strip_extension()    noexcept;
    static int                get_source_detail_mode() noexcept;
    static const std::string& get_file_pattern()       noexcept;
    static const std::string& get_console_pattern()    noexcept;

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    /// @brief Get the shared spdlog::logger instance. Initializes on first call.
    static std::shared_ptr<spdlog::logger> get();

    /// @brief Flush and release the logger. Safe to call multiple times.
    static void shutdown();

    /// @name Formatted logging (used by LOG_* macros — prefer macros over direct calls)
    /// @{

    /// @brief Core logging function. Formats message, applies to_loggable(), writes to sinks.
    /// @tparam Lvl   spdlog level (compile-time dispatch for NDEBUG elision).
    /// @param file   Source file (__FILE__).
    /// @param line   Source line (__LINE__).
    /// @param fmt_str  fmt::format_string with compile-time format checking.
    template<spdlog::level::level_enum Lvl, typename... Args>
    static void log_fmt(std::string_view file, int line,
                        fmt::format_string<Args...> fmt_str, Args&&... args)
    {
        if (!should_log(Lvl)) return;
        try {
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
    /// @}

private:
    // ── Configuration (private) ─────────────────────────────────────────────
    static std::string log_level_;
    static std::string log_file_;
    static size_t      max_size_;
    static int         max_files_;
    static std::string project_root_;
    static bool        strip_extension_;
    static int         source_detail_mode_;
    static std::string file_pattern_;
    static std::string console_pattern_;

    static std::once_flag init_flag_;

    // ── Meyers Singleton storage ──────────────────────────────────────────────
    static std::shared_ptr<spdlog::logger>& get_instance() noexcept;

    static void ensure_initialized();
    static void init_internal();
    static bool should_log(spdlog::level::level_enum lvl);
    static void log_raw(spdlog::source_loc loc, spdlog::level::level_enum lvl, std::string_view msg);
};

// ─── ScopedLog (debug only, zero-overhead template dispatch) ─────────────────

/// @brief RAII scope tracer that logs ">>> func" on entry and "<<< func" on exit.
///
/// Template parameter selects the log level at compile time — in Release builds
/// (NDEBUG), ScopedLog is not defined and the macros expand to `((void)0)`.
///
/// Usage (via macros):
/// @code
///   void ConnectionManager::shutdown() {
///       LOG_SCOPE_DEBUG();  // logs >>> shutdown ... <<< shutdown
///       // ...
///   }
/// @endcode
#ifndef NDEBUG
template<spdlog::level::level_enum Lvl>
class ScopedLog {
public:
    ScopedLog(const char* fn, const char* file, int line)
        : fn_(fn), file_(file), line_(line)
    {
        Logger::log_fmt<Lvl>(file_, line_, ">>> {}", fn_);
    }
    ~ScopedLog() {
        Logger::log_fmt<Lvl>(file_, line_, "<<< {}", fn_);
    }
private:
    const char* fn_;
    const char* file_;
    int         line_;
};
#endif

// ─── Log macros ──────────────────────────────────────────────────────────────
/// @name Primary log macros
/// Usage: `LOG_INFO("peer {} connected", peer_id);`
///
/// TRACE/DEBUG/SCOPE macros compile to `((void)0)` in Release (NDEBUG).
/// INFO/WARN/ERROR/CRITICAL are always active.
/// @{

#ifdef NDEBUG
    #define LOG_TRACE(...)         ((void)0)
    #define LOG_DEBUG(...)         ((void)0)
    #define LOG_SCOPE_TRACE()      ((void)0)
    #define LOG_SCOPE_DEBUG()      ((void)0)
#else
    #define LOG_TRACE(...) Logger::trace(__FILE__, __LINE__, __VA_ARGS__)
    #define LOG_DEBUG(...) Logger::debug(__FILE__, __LINE__, __VA_ARGS__)

    /// @brief RAII scope tracer at TRACE level.
    #define LOG_SCOPE_TRACE() \
        ScopedLog<spdlog::level::trace> _gn_scope_##__LINE__(__FUNCTION__, __FILE__, __LINE__)
    /// @brief RAII scope tracer at DEBUG level.
    #define LOG_SCOPE_DEBUG() \
        ScopedLog<spdlog::level::debug> _gn_scope_##__LINE__(__FUNCTION__, __FILE__, __LINE__)
#endif

#define LOG_INFO(...)     Logger::info    (__FILE__, __LINE__, __VA_ARGS__)
#define LOG_WARN(...)     Logger::warn    (__FILE__, __LINE__, __VA_ARGS__)
#define LOG_ERROR(...)    Logger::error   (__FILE__, __LINE__, __VA_ARGS__)
#define LOG_CRITICAL(...) Logger::critical(__FILE__, __LINE__, __VA_ARGS__)
/// @}

// ── Value / Pointer tracing macros ───────────────────────────────────────────
/// @name Value/Pointer tracing
/// Quick-trace macros for variables and pointers.
///
/// `TRACE_*` / `DEBUG_*` — zero-cost in Release (NDEBUG).
/// `INFO_*`  / `WARN_*` / `ERROR_*` — always active.
///
/// Usage:
/// @code
///   TRACE_VALUE("conn_id", id);            // "conn_id = 42"
///   TRACE_VALUE_DETAILED("buf_sz", sz);    // "[file:123] buf_sz = 1024"
///   DEBUG_POINTER("session", sess.get());  // "session = 0x7fff..."
///   ERROR_VALUE("errno", ec);              // always logged
/// @endcode
/// @{

#ifdef NDEBUG
    #define TRACE_VALUE(name, val)          ((void)0)
    #define TRACE_VALUE_DETAILED(name, val) ((void)0)
    #define TRACE_POINTER(name, ptr)        ((void)0)
    #define DEBUG_VALUE(name, val)          ((void)0)
    #define DEBUG_VALUE_DETAILED(name, val) ((void)0)
    #define DEBUG_POINTER(name, ptr)        ((void)0)
#else
    #define TRACE_VALUE(name, val) \
        LOG_TRACE("{} = {}", name, val)
    #define TRACE_VALUE_DETAILED(name, val) \
        LOG_TRACE("[{}:{}] {} = {}", __FILE__, __LINE__, name, val)
    #define TRACE_POINTER(name, ptr) \
        LOG_TRACE("{} = {}", name, static_cast<const void*>(ptr))
    #define DEBUG_VALUE(name, val) \
        LOG_DEBUG("{} = {}", name, val)
    #define DEBUG_VALUE_DETAILED(name, val) \
        LOG_DEBUG("[{}:{}] {} = {}", __FILE__, __LINE__, name, val)
    #define DEBUG_POINTER(name, ptr) \
        LOG_DEBUG("{} = {}", name, static_cast<const void*>(ptr))
#endif

#define INFO_VALUE(name, val)    LOG_INFO("{} = {}", name, val)
#define WARN_VALUE(name, val)    LOG_WARN("{} = {}", name, val)
#define ERROR_VALUE(name, val)   LOG_ERROR("{} = {}", name, val)
#define INFO_POINTER(name, ptr)  LOG_INFO("{} = {}", name, static_cast<const void*>(ptr))
#define WARN_POINTER(name, ptr)  LOG_WARN("{} = {}", name, static_cast<const void*>(ptr))
#define ERROR_POINTER(name, ptr) LOG_ERROR("{} = {}", name, static_cast<const void*>(ptr))
/// @}
