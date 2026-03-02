#include "logger.hpp"
#include <filesystem>
#include <iostream>
#include <spdlog/pattern_formatter.h>
#include <spdlog/details/log_msg.h>
#include <string_view>
#include <mutex>

namespace fs = std::filesystem;

// ─── Статические переменные ───────────────────────────────────────────────────

std::shared_ptr<spdlog::logger> Logger::logger_ = nullptr;
std::string  Logger::log_level          = "info";
std::string  Logger::log_file           = "logs/goodnet.log";
size_t       Logger::max_size           = 10 * 1024 * 1024;
int          Logger::max_files          = 5;
bool         Logger::strip_extension    = false;
int          Logger::source_detail_mode = 0;
std::string  Logger::file_pattern       = "[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %Q%v";
std::string  Logger::console_pattern    = "%^[%Y-%m-%d %H:%M:%S.%e] [%l] %Q%$%v";

// project_root: корень проекта для отображения относительных путей в логах.
//
// Если задан CMake define GOODNET_PROJECT_ROOT (через target_compile_definitions),
// используем его как дефолт. Это даёт пути вида [src/config.cpp:42] вместо [config.cpp:42].
//
// Можно переопределить в рантайме: Logger::project_root = "/my/project";
// Пустая строка = только basename файла.
#ifdef GOODNET_PROJECT_ROOT
std::string  Logger::project_root = GOODNET_PROJECT_ROOT;
#else
std::string  Logger::project_root = "";
#endif

std::once_flag Logger::init_flag;

// ─── Кастомный флаг %Q ───────────────────────────────────────────────────────
//
// Форматирует источник сообщения с учётом source_detail_mode и project_root:
//
//   mode 0 (авто):    trace/debug → [путь/к/файлу:строка]
//                     info+       → [файл.cpp]
//   mode 1 (максимум): [путь/к/файлу:строка]
//   mode 2 (средний):  [файл.cpp:строка]
//   mode 3 (минимум):  [файл.cpp]  или [файл] если strip_extension=true
//
// Если project_root = "/home/user/GoodNet" и __FILE__ = "/home/user/GoodNet/src/config.cpp"
// → rel = "src/config.cpp"
//
// Если project_root = "" → берётся только basename: "config.cpp"

class custom_source_flag : public spdlog::custom_flag_formatter {
public:
    void format(const spdlog::details::log_msg& msg,
                const std::tm&,
                spdlog::memory_buf_t& dest) override
    {
        if (msg.source.empty()) return;

        std::string_view full_path(msg.source.filename);
        std::string_view rel = full_path;

        // ── Шаг 1: сделать путь относительным ────────────────────────────────
        if (!Logger::project_root.empty()) {
            std::string_view root(Logger::project_root);
            if (full_path.starts_with(root)) {
                rel = full_path.substr(root.size());
                // Убрать ведущий слэш
                if (!rel.empty() && (rel[0] == '/' || rel[0] == '\\'))
                    rel = rel.substr(1);
            }
            // Если путь не совпадает — оставить полный (не ломать вывод)
        } else {
            // Без project_root — только basename
            auto slash = rel.find_last_of("/\\");
            if (slash != std::string_view::npos)
                rel = rel.substr(slash + 1);
        }

        // ── Шаг 2: убрать расширение ──────────────────────────────────────────
        std::string_view name = rel;
        if (Logger::strip_extension) {
            auto dot = name.find_last_of('.');
            if (dot != std::string_view::npos)
                name = name.substr(0, dot);
        }

        // ── Шаг 3: детализация по mode ────────────────────────────────────────
        const bool is_verbose = (msg.level == spdlog::level::trace ||
                                  msg.level == spdlog::level::debug);

        bool show_path = false, show_line = false;
        switch (Logger::source_detail_mode) {
            case 0: show_path = is_verbose; show_line = is_verbose; break;
            case 1: show_path = true;  show_line = true;  break;
            case 2: show_path = false; show_line = true;  break;
            case 3: show_path = false; show_line = false; break;
            default: break;
        }

        // show_path=true → rel (относительный путь), false → name (basename ± расширение)
        std::string_view display = show_path ? rel : name;

        if (show_line)
            fmt::format_to(std::back_inserter(dest), "[{}:{}] ", display, msg.source.line);
        else
            fmt::format_to(std::back_inserter(dest), "[{}] ", display);
    }

    std::unique_ptr<custom_flag_formatter> clone() const override {
        return std::make_unique<custom_source_flag>();
    }
};

// ─── ensure_initialized ──────────────────────────────────────────────────────
//
// Вызывается из каждого LOG_* макроса, в том числе из плагинов.
//
// Плагины загружаются с RTLD_GLOBAL — они видят символы libgoodnet_core.so
// (или исполняемого файла с -rdynamic). Но их Logger::logger_ — отдельная
// копия статической переменной из их TU. Поэтому используем глобальный
// реестр spdlog как bridge между ядром и плагинами.

void Logger::ensure_initialized() {
    if (logger_) return;

    // Сначала ищем в реестре (на случай, если плагин всё же видит символы ядра)
    auto existing = spdlog::get("goodnet");
    if (existing) {
        logger_ = existing;
        return;
    }

    // Если мы в ядре и логгер еще не создан — инициализируем
    std::call_once(init_flag, []() { init_internal(); });
}

// ─── init_internal ───────────────────────────────────────────────────────────

void Logger::init_internal() {
    try {
        fs::path log_path = log_file;
        if (!log_path.parent_path().empty())
            fs::create_directories(log_path.parent_path());

        std::vector<spdlog::sink_ptr> sinks;

        // Файловый синк с ротацией
        {
            auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                log_file, max_size, max_files);
            auto fmt = std::make_unique<spdlog::pattern_formatter>();
            fmt->add_flag<custom_source_flag>('Q');
            fmt->set_pattern(file_pattern);
            sink->set_formatter(std::move(fmt));
            sinks.push_back(std::move(sink));
        }

        // Консольный синк — только в debug-сборке
#ifndef NDEBUG
        {
            auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            auto fmt  = std::make_unique<spdlog::pattern_formatter>();
            fmt->add_flag<custom_source_flag>('Q');
            fmt->set_pattern(console_pattern);
            sink->set_formatter(std::move(fmt));
            sinks.push_back(std::move(sink));
        }
#endif

        logger_ = std::make_shared<spdlog::logger>("goodnet",
                                                    sinks.begin(), sinks.end());

        // Уровень
        const auto lvl = [&]() -> spdlog::level::level_enum {
            if (log_level == "trace")    return spdlog::level::trace;
            if (log_level == "debug")    return spdlog::level::debug;
            if (log_level == "warn")     return spdlog::level::warn;
            if (log_level == "error")    return spdlog::level::err;
            if (log_level == "critical") return spdlog::level::critical;
            if (log_level == "off")      return spdlog::level::off;
            return spdlog::level::info;
        }();
        logger_->set_level(lvl);
        logger_->flush_on(spdlog::level::warn);
        spdlog::flush_every(std::chrono::seconds(3));

        // Регистрация в реестре spdlog — обязательно для плагинов.
        // Плагин вызывает spdlog::get("goodnet") → получает тот же экземпляр.
        if (spdlog::get("goodnet"))
            spdlog::drop("goodnet");
        spdlog::register_logger(logger_);

        logger_->info("Logger initialized. level={} mode={} root='{}'",
                      log_level, source_detail_mode,
                      project_root.empty() ? "(basename only)" : project_root);

    } catch (const spdlog::spdlog_ex& ex) {
        std::cerr << "[Logger] init failed: " << ex.what() << '\n';
        throw;
    }
}

// ─── shutdown ────────────────────────────────────────────────────────────────

void Logger::shutdown() {
    if (!logger_) return;
    logger_->info("Logger shutting down");
    logger_->flush();
    spdlog::drop_all();
    logger_.reset();
}

void Logger::log_raw(spdlog::source_loc loc, spdlog::level::level_enum lvl, std::string_view msg) {
    if (logger_) {
        // Передаем "{}", чтобы spdlog не пытался искать форматирование внутри msg
        logger_->log(loc, lvl, "{}", msg);
    }
}
