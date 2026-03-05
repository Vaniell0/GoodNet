#include "logger.hpp"

// ─── Тяжёлые включения ТОЛЬКО здесь ─────────────────────────────────────────
// spdlog/spdlog.h (~1000 строк + зависимости) не попадает в публичный хедер.
// Все TU, включающие logger.hpp, компилируются без этой нагрузки.
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/pattern_formatter.h>

#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

// ─── Статические переменные ───────────────────────────────────────────────────

std::string   Logger::log_level          = "info";
std::string   Logger::log_file           = "logs/goodnet.log";
size_t        Logger::max_size           = 10 * 1024 * 1024;
int           Logger::max_files          = 5;
bool          Logger::strip_extension    = false;
int           Logger::source_detail_mode = 0;
std::string   Logger::file_pattern       = "[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %Q%v";
std::string   Logger::console_pattern    = "%^[%Y-%m-%d %H:%M:%S.%e] [%l] %Q%$%v";

#ifdef GOODNET_PROJECT_ROOT
std::string   Logger::project_root = GOODNET_PROJECT_ROOT;
#else
std::string   Logger::project_root = "";
#endif

std::once_flag Logger::init_flag;

// ─── Meyers Singleton storage ────────────────────────────────────────────────
//
// Хранение shared_ptr<spdlog::logger> как static local:
//
//   Плюсы Meyers Singleton над глобальным static:
//   • Инициализируется при ПЕРВОМ обращении, а не при старте программы.
//     Нет "инициализация до main()" — нет SIOF (Static Initialization Order Fiasco).
//   • Разрушается в обратном порядке инициализации — предсказуемо.
//   • shutdown() явно обнуляет ptr: деструктор static local при выходе программы
//     видит nullptr → _M_dispose НЕ вызывается → нет SIGSEGV.
//
//   Почему static local, а не просто static member:
//   • Статический член logger_ инициализируется вместе с другими статиками TU.
//     Порядок инициализации между TU не определён → потенциальный SIOF.
//   • Локальный static функции инициализируется при первом вызове функции.
//     std::call_once + Meyers = потокобезопасность по C++11 стандарту.

std::shared_ptr<spdlog::logger>& Logger::get_instance() noexcept {
    static std::shared_ptr<spdlog::logger> instance;
    return instance;
}

// ─── Кастомный флаг %Q ───────────────────────────────────────────────────────
//
// Форматирует источник сообщения:
//   mode 0 (авто):     trace/debug → [путь:строка], info+ → [файл.cpp]
//   mode 1 (полный):   [путь/к/файлу:строка]
//   mode 2 (средний):  [файл.cpp:строка]
//   mode 3 (минимум):  [файл.cpp]
//
// project_root: если задан, обрезает абсолютный путь до относительного.
// "" → только basename.

class custom_source_flag : public spdlog::custom_flag_formatter {
public:
    void format(const spdlog::details::log_msg& msg,
                const std::tm&,
                spdlog::memory_buf_t& dest) override
    {
        if (msg.source.empty()) return;

        std::string_view full(msg.source.filename);
        std::string_view rel = full;

        // Шаг 1: относительный путь
        if (!Logger::project_root.empty()) {
            std::string_view root(Logger::project_root);
            if (full.starts_with(root)) {
                rel = full.substr(root.size());
                if (!rel.empty() && (rel[0] == '/' || rel[0] == '\\'))
                    rel = rel.substr(1);
            }
        } else {
            auto s = rel.find_last_of("/\\");
            if (s != std::string_view::npos) rel = rel.substr(s + 1);
        }

        // Шаг 2: strip extension
        std::string_view name = rel;
        if (Logger::strip_extension) {
            auto d = name.find_last_of('.');
            if (d != std::string_view::npos) name = name.substr(0, d);
        }

        // Шаг 3: уровень детализации
        const bool verbose = (msg.level == spdlog::level::trace ||
                               msg.level == spdlog::level::debug);
        bool show_path = false, show_line = false;
        switch (Logger::source_detail_mode) {
            case 0: show_path = verbose; show_line = verbose; break;
            case 1: show_path = true;  show_line = true;  break;
            case 2: show_path = false; show_line = true;  break;
            case 3: show_path = false; show_line = false; break;
        }

        const std::string_view display = show_path ? rel : name;
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

void Logger::ensure_initialized() {
    if (get_instance()) return;
    std::call_once(init_flag, []() { init_internal(); });
}

// ─── get ─────────────────────────────────────────────────────────────────────

std::shared_ptr<spdlog::logger> Logger::get() {
    ensure_initialized();
    return get_instance();
}

// ─── set_external_logger ─────────────────────────────────────────────────────

void Logger::set_external_logger(std::shared_ptr<spdlog::logger> ext) {
    get_instance() = std::move(ext);
}

// ─── init_internal ───────────────────────────────────────────────────────────

void Logger::init_internal() {
    try {
        // Создаём папку для лога
        fs::path log_path = log_file;
        if (!log_path.parent_path().empty())
            fs::create_directories(log_path.parent_path());

        std::vector<spdlog::sink_ptr> sinks;

        {
            auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                log_file, max_size, max_files);
            auto fmt = std::make_unique<spdlog::pattern_formatter>();
            fmt->add_flag<custom_source_flag>('Q');
            fmt->set_pattern(file_pattern);
            sink->set_formatter(std::move(fmt));
            sinks.push_back(std::move(sink));
        }

        // Консоль — только в Debug
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

        auto logger = std::make_shared<spdlog::logger>("goodnet",
                                                        sinks.begin(), sinks.end());

        const auto lvl = [&]() -> spdlog::level::level_enum {
            if (log_level == "trace")    return spdlog::level::trace;
            if (log_level == "debug")    return spdlog::level::debug;
            if (log_level == "warn")     return spdlog::level::warn;
            if (log_level == "error")    return spdlog::level::err;
            if (log_level == "critical") return spdlog::level::critical;
            if (log_level == "off")      return spdlog::level::off;
            return spdlog::level::info;
        }();
        logger->set_level(lvl);
        logger->flush_on(spdlog::level::warn);
        spdlog::flush_every(std::chrono::seconds(3));

        // Регистрируем в глобальном реестре spdlog — fallback для плагинов
        // без внешнего логгера (если sync_plugin_context не был вызван)
        if (spdlog::get("goodnet")) spdlog::drop("goodnet");
        spdlog::register_logger(logger);

        get_instance() = logger;

        logger->info("Logger initialized. level={} mode={} root='{}'",
                     log_level, source_detail_mode,
                     project_root.empty() ? "(basename only)" : project_root);

    } catch (const spdlog::spdlog_ex& ex) {
        std::cerr << "[Logger] init failed: " << ex.what() << '\n';
        throw;
    }
}

// ─── shutdown ────────────────────────────────────────────────────────────────
//
// Детерминированный порядок завершения:
//   1. logger->flush()         — сброс буферов
//   2. spdlog::drop_all()      — удаляем из реестра (spdlog не держит ref)
//   3. get_instance().reset()  — shared_ptr = nullptr
//
// После reset() Meyers static local = nullptr.
// При __do_global_dtors_aux: static local dtor видит nullptr → _M_dispose НЕ вызывается.
// SIGSEGV "shared_ptr _M_release → _M_dispose на мёртвом объекте" устранён.

void Logger::shutdown() {
    auto& inst = get_instance();
    if (!inst) return;
    inst->info("Logger shutting down");
    inst->flush();
    spdlog::drop_all();
    inst.reset();   // ← ключевой момент: обнуляем до выгрузки .so
}

// ─── should_log / log_raw ────────────────────────────────────────────────────

bool Logger::should_log(spdlog::level::level_enum lvl) {
    ensure_initialized();
    return get_instance() && get_instance()->should_log(lvl);
}

void Logger::log_raw(spdlog::source_loc loc, spdlog::level::level_enum lvl, std::string_view msg) {
    auto& inst = get_instance();
    if (inst) inst->log(loc, lvl, "{}", msg);
}