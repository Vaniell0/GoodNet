#pragma once

#include <mutex>
#include <string>
#include <chrono>
#include <iomanip>
#include <fstream>
#include <type_traits>

#include "fmt/color.h"
#include "fmt/format.h"
#include "fmt/ranges.h"
#include "fmt/chrono.h"
#include "fmt/compile.h"

/**
  * @brief Уровни детализации логов приложения.
*/
enum class LogLevel {
    /// Максимально подробная информация для отладки потоков данных и состояний (dump пакетов, шаги циклов).
    TRACE,

    /// Информация, полезная при разработке и поиске багов (вход в функции, значения параметров).
    DEBUG,

    /// Стандартные уведомления о нормальной работе систем (запуск сервисов, подключение плагина).
    INFO,

    /// Предупреждения о некритичных аномалиях, которые не мешают работе (повторная попытка запроса).
    WARNING,

    /// Ошибки выполнения конкретных операций (отказ в доступе, обрыв соединения), ядро продолжает работать.
    ERROR,

    /// Критические сбои, требующие немедленного внимания или приводящие к остановке приложения (отсутствие SDK, краш потока).
    CRITICAL
};

// Глобальный уровень логирования
inline LogLevel LOG_LEVEL = LogLevel::INFO;

class Logger {
public:
    Logger(std::string module_name, 
           LogLevel min_level = LogLevel::INFO,
           bool use_colors = true,
           bool use_timestamps = true);

    // Базовый метод логирования
    template<typename... Args>
    void log(LogLevel level, fmt::format_string<Args...> fmt, Args&&... args) {
        if (level < min_level_ && !(level == LogLevel::TRACE)) {
            return;
        }

        std::string message = format_message(level, fmt, std::forward<Args>(args)...);
        
        std::lock_guard<std::mutex> lock(log_mutex_);
        output_to_console(level, message);
        output_to_file(message);
    }

    // Упрощенные методы для каждого уровня
    template<typename... Args>
    void trace(fmt::format_string<Args...> fmt, Args&&... args) { 
        log(LogLevel::TRACE, fmt, std::forward<Args>(args)...); 
    }
    
    template<typename... Args>
    void debug(fmt::format_string<Args...> fmt, Args&&... args) { 
        log(LogLevel::DEBUG, fmt, std::forward<Args>(args)...); 
    }
    
    template<typename... Args>
    void info(fmt::format_string<Args...> fmt, Args&&... args) { 
        log(LogLevel::INFO, fmt, std::forward<Args>(args)...); 
    }
    
    template<typename... Args>
    void warning(fmt::format_string<Args...> fmt, Args&&... args) { 
        log(LogLevel::WARNING, fmt, std::forward<Args>(args)...); 
    }
    
    template<typename... Args>
    void error(fmt::format_string<Args...> fmt, Args&&... args) { 
        log(LogLevel::ERROR, fmt, std::forward<Args>(args)...); 
    }
    
    template<typename... Args>
    void critical(fmt::format_string<Args...> fmt, Args&&... args) { 
        log(LogLevel::CRITICAL, fmt, std::forward<Args>(args)...); 
    }

    // Методы для трассировки вызовов функций
    void trace_enter(const std::string& function_name, const std::string& args = "");
    void trace_exit(const std::string& function_name, const std::string& result = "");

    // Проверка уровня логирования
    bool is_trace_enabled() const { return min_level_ <= LogLevel::TRACE; }
    bool is_debug_enabled() const { return min_level_ <= LogLevel::DEBUG; }
    bool is_info_enabled() const { return min_level_ <= LogLevel::INFO; }

    // Статические методы для управления логгированием
    static void enable_file_logging(const std::string& filename);
    static void disable_file_logging();
    static std::string get_log_file_path();
    static bool is_file_logging_enabled();

private:
    std::string module_name_;
    LogLevel min_level_;
    bool use_colors_;
    bool use_timestamps_;
    
    static std::mutex log_mutex_;
    static std::ofstream log_file_;
    static bool file_logging_enabled_;
    static std::string log_file_path_;

    // Получение цвета fmt
    fmt::text_style get_fmt_style(LogLevel level) const;
    static const char* level_to_str(LogLevel level);

    // Вспомогательная функция для форматирования
    template<typename... Args>
    std::string format_message(LogLevel level, fmt::format_string<Args...> fmt, Args&&... args) {
        // Преобразуем аргументы для безопасного форматирования
        std::string user_msg;
        try {
            user_msg = fmt::format(fmt, std::forward<Args>(args)...);
        } catch (const fmt::format_error& e) {
            user_msg = fmt::format("Format error: {} | Original args count: {}", e.what(), sizeof...(Args));
        }
        
        // Собираем полное сообщение
        std::ostringstream oss;
        
        // Время
        if (use_timestamps_) {
            auto now = std::chrono::system_clock::now();
            auto now_time_t = std::chrono::system_clock::to_time_t(now);
            auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()) % 1000;
            
            std::tm now_tm;
            localtime_r(&now_time_t, &now_tm);
            oss << std::put_time(&now_tm, "%Y-%m-%d %H:%M:%S") 
                << '.' << std::setfill('0') << std::setw(3) << now_ms.count() << ' ';
        }

        // Уровень
        oss << '[' << level_to_str(level) << "] ";

        // Модуль и сообщение
        oss << '[' << module_name_ << "] " << user_msg;

        return oss.str();
    }

    // Вывод в консоль с цветами
    void output_to_console(LogLevel level, const std::string& message);

    // Вывод в файл (без цветов)
    void output_to_file(const std::string& message);
};

void set_log_level(const LogLevel& log_level);

// Макросы для автоматического создания логгеров
#define LOGGER(module_name) \
    static Logger logger(module_name, LOG_LEVEL)

#define LOGGER_AUTO() \
    static Logger logger(std::filesystem::path(__FILE__).stem().string(), LOG_LEVEL)

// Макросы для трассировки входа/выхода из функций
#define LOG_TRACE_ENTER() \
    if (LOG_LEVEL <= LogLevel::TRACE) { \
        Logger(__FUNCTION__, LogLevel::TRACE).trace_enter(__FUNCTION__); \
    }

#define LOG_TRACE_ENTER_ARGS(...) \
    if (LOG_LEVEL <= LogLevel::TRACE) { \
        std::ostringstream args_oss; \
        args_oss << #__VA_ARGS__; \
        Logger(__FUNCTION__, LogLevel::TRACE).trace_enter(__FUNCTION__, args_oss.str()); \
    }

#define LOG_TRACE_EXIT() \
    if (LOG_LEVEL <= LogLevel::TRACE) { \
        Logger(__FUNCTION__, LogLevel::TRACE).trace_exit(__FUNCTION__); \
    }

#define LOG_TRACE_EXIT_VALUE(value) \
    if (LOG_LEVEL <= LogLevel::TRACE) { \
        std::ostringstream value_oss; \
        value_oss << value; \
        Logger(__FUNCTION__, LogLevel::TRACE).trace_exit(__FUNCTION__, value_oss.str()); \
    }
