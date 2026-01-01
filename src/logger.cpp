#include "logger.hpp"

Logger::Logger(std::string module_name,
        LogLevel min_level,
        bool use_colors,
        bool use_timestamps)
    : module_name_(std::move(module_name))
    , min_level_(min_level)
    , use_colors_(use_colors)
    , use_timestamps_(use_timestamps) {}

// Методы для трассировки вызовов функций
void Logger::trace_enter(const std::string& function_name, const std::string& args) {
    if (min_level_ <= LogLevel::TRACE) {
        if (args.empty()) {
            trace("--> {}", function_name);
        } else {
            trace("--> {}({})", function_name, args);
        }
    }
}

void Logger::trace_exit(const std::string& function_name, const std::string& result) {
    if (min_level_ <= LogLevel::TRACE) {
        if (result.empty()) {
            trace("<-- {}", function_name);
        } else {
            trace("<-- {} -> {}", function_name, result);
        }
    }
}

// Статические методы для управления логгированием
void Logger::enable_file_logging(const std::string& filename) {
    std::lock_guard<std::mutex> lock(log_mutex_);
    
    if (log_file_.is_open()) {
        log_file_.close();
    }
    
    log_file_.open(filename, std::ios::app);
    file_logging_enabled_ = log_file_.is_open();
    log_file_path_ = filename;
    
    if (file_logging_enabled_) {
        auto now = std::chrono::system_clock::now();
        auto now_time_t = std::chrono::system_clock::to_time_t(now);
        std::tm now_tm;
        localtime_r(&now_time_t, &now_tm);
        log_file_ << "\n\n=== Logging started at: " 
                  << std::put_time(&now_tm, "%Y-%m-%d %H:%M:%S")
                  << " ===\n";
    }
}

void Logger::disable_file_logging() {
    std::lock_guard<std::mutex> lock(log_mutex_);
    if (log_file_.is_open()) {
        log_file_.close();
    }
    file_logging_enabled_ = false;
}

std::string Logger::get_log_file_path() {
    std::lock_guard<std::mutex> lock(log_mutex_);
    return log_file_path_;
}

bool Logger::is_file_logging_enabled() {
    std::lock_guard<std::mutex> lock(log_mutex_);
    return file_logging_enabled_;
}

// Получение цвета fmt
fmt::text_style Logger::get_fmt_style(LogLevel level) const {
    switch (level) {
        case LogLevel::TRACE:    return fmt::fg(fmt::color::gray);
        case LogLevel::DEBUG:    return fmt::fg(fmt::color::cyan);
        case LogLevel::INFO:     return fmt::fg(fmt::color::green);
        case LogLevel::WARNING:  return fmt::fg(fmt::color::yellow);
        case LogLevel::ERROR:    return fmt::fg(fmt::color::red);
        case LogLevel::CRITICAL: return fmt::fg(fmt::color::magenta) | fmt::emphasis::bold;
        default:                 return {};
    }
}

const char* Logger::level_to_str(LogLevel level) {
    switch (level) {
        case LogLevel::TRACE:    return "TRACE";
        case LogLevel::DEBUG:    return "DEBUG";
        case LogLevel::INFO:     return "INFO";
        case LogLevel::WARNING:  return "WARN";
        case LogLevel::ERROR:    return "ERROR";
        case LogLevel::CRITICAL: return "CRIT";
        default:                 return "UNKNOWN";
    }
}

// Вывод в консоль с цветами
void Logger::output_to_console(LogLevel level, const std::string& message) {
    if (use_colors_) {
        fmt::print(stderr, get_fmt_style(level), "{}\n", message);
    } else {
        fmt::print(stderr, "{}\n", message);
    }
}

// Вывод в файл (без цветов)
void Logger::output_to_file(const std::string& message) {
    if (file_logging_enabled_ && log_file_.is_open()) {
        log_file_ << message << std::endl;
        log_file_.flush();
    }
}

// Инициализация статических членов
std::mutex Logger::log_mutex_;
std::ofstream Logger::log_file_;
bool Logger::file_logging_enabled_ = false;
std::string Logger::log_file_path_;

void set_log_level(const LogLevel& log_level) {
    LOG_LEVEL = log_level;
}
