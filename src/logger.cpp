#include <spdlog/spdlog.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include "logger.hpp"

#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

// ─── Статические переменные (private) ────────────────────────────────────────

std::string   Logger::log_level_          = "info";
std::string   Logger::log_file_           = "";
size_t        Logger::max_size_           = 10 * 1024 * 1024;
int           Logger::max_files_          = 5;
bool          Logger::strip_extension_    = false;
int           Logger::source_detail_mode_ = 0;
std::string   Logger::file_pattern_       = "[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %Q%v";
std::string   Logger::console_pattern_    = "%^[%Y-%m-%d %H:%M:%S.%e] [%l] %Q%$%v";

#ifdef GOODNET_PROJECT_ROOT
std::string   Logger::project_root_ = GOODNET_PROJECT_ROOT;
#else
std::string   Logger::project_root_ = "";
#endif

std::once_flag Logger::init_flag_;

// ─── Helper: string → spdlog level ─────────────────────────────────────────

static spdlog::level::level_enum parse_level(std::string_view s) {
    if (s == "trace")    return spdlog::level::trace;
    if (s == "debug")    return spdlog::level::debug;
    if (s == "warn" || s == "warning") return spdlog::level::warn;
    if (s == "error")    return spdlog::level::err;
    if (s == "critical") return spdlog::level::critical;
    if (s == "off")      return spdlog::level::off;
    return spdlog::level::info;
}

// ─── Сеттеры ─────────────────────────────────────────────────────────────────

void Logger::set_log_level(std::string_view level)        { log_level_ = std::string(level); }
void Logger::set_log_file(std::string_view path)          { log_file_ = std::string(path); }
void Logger::set_max_size(size_t bytes)                   { max_size_ = bytes; }
void Logger::set_max_files(int count)                     { max_files_ = count; }
void Logger::set_project_root(std::string_view root)      { project_root_ = std::string(root); }
void Logger::set_strip_extension(bool strip)              { strip_extension_ = strip; }
void Logger::set_source_detail_mode(int mode)             { source_detail_mode_ = mode; }
void Logger::set_file_pattern(std::string_view pattern)   { file_pattern_ = std::string(pattern); }
void Logger::set_console_pattern(std::string_view pattern){ console_pattern_ = std::string(pattern); }

// ─── Геттеры ─────────────────────────────────────────────────────────────────

const std::string& Logger::get_log_level()          noexcept { return log_level_; }
const std::string& Logger::get_log_file()           noexcept { return log_file_; }
size_t             Logger::get_max_size()            noexcept { return max_size_; }
int                Logger::get_max_files()           noexcept { return max_files_; }
const std::string& Logger::get_project_root()       noexcept { return project_root_; }
bool               Logger::get_strip_extension()    noexcept { return strip_extension_; }
int                Logger::get_source_detail_mode() noexcept { return source_detail_mode_; }
const std::string& Logger::get_file_pattern()       noexcept { return file_pattern_; }
const std::string& Logger::get_console_pattern()    noexcept { return console_pattern_; }

// ─── Meyers Singleton storage ───────────────────────────────────────────────

std::shared_ptr<spdlog::logger>& Logger::get_instance() noexcept {
    static std::shared_ptr<spdlog::logger> instance;
    return instance;
}

// ─── Кастомный флаг %Q ─────────────────────────────────────────────────────

class custom_source_flag : public spdlog::custom_flag_formatter {
public:
    void format(const spdlog::details::log_msg& msg,
                const std::tm&,
                spdlog::memory_buf_t& dest) override
    {
        if (msg.source.empty()) return;

        std::string_view full(msg.source.filename);
        std::string_view rel = full;

        const auto& root = Logger::get_project_root();
        if (!root.empty()) {
            if (full.starts_with(root)) {
                rel = full.substr(root.size());
                if (!rel.empty() && (rel[0] == '/' || rel[0] == '\\'))
                    rel = rel.substr(1);
            }
        } else {
            auto s = rel.find_last_of("/\\");
            if (s != std::string_view::npos) rel = rel.substr(s + 1);
        }

        std::string_view name = rel;
        if (Logger::get_strip_extension()) {
            auto d = name.find_last_of('.');
            if (d != std::string_view::npos) name = name.substr(0, d);
        }

        const bool verbose = (msg.level == spdlog::level::trace ||
                               msg.level == spdlog::level::debug);
        bool show_path = false, show_line = false;
        switch (Logger::get_source_detail_mode()) {
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

// ─── ensure_initialized ─────────────────────────────────────────────────────

void Logger::ensure_initialized() {
    if (get_instance()) return;
    std::call_once(init_flag_, []() { init_internal(); });
}

// ─── get ─────────────────────────────────────────────────────────────────────

std::shared_ptr<spdlog::logger> Logger::get() {
    ensure_initialized();
    return get_instance();
}

// ─── init_internal ──────────────────────────────────────────────────────────

void Logger::init_internal() {
    try {
        std::vector<spdlog::sink_ptr> sinks;

        // File sink (only if log_file_ is set)
        if (!log_file_.empty()) {
            fs::path log_path = log_file_;
            if (!log_path.parent_path().empty())
                fs::create_directories(log_path.parent_path());

            auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                log_file_, max_size_, max_files_);
            auto fmt = std::make_unique<spdlog::pattern_formatter>();
            fmt->add_flag<custom_source_flag>('Q');
            fmt->set_pattern(file_pattern_);
            sink->set_formatter(std::move(fmt));
            sinks.push_back(std::move(sink));
        }

        // Консоль: Debug — полный вывод, Release — только warn+
        {
            auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            auto fmt  = std::make_unique<spdlog::pattern_formatter>();
            fmt->add_flag<custom_source_flag>('Q');
            fmt->set_pattern(console_pattern_);
            sink->set_formatter(std::move(fmt));
#ifdef NDEBUG
            sink->set_level(spdlog::level::warn);
#endif
            sinks.push_back(std::move(sink));
        }

        auto logger = std::make_shared<spdlog::logger>("goodnet",
                                                        sinks.begin(), sinks.end());

        const auto lvl = parse_level(log_level_);
        logger->set_level(lvl);
        logger->flush_on(spdlog::level::warn);
        spdlog::flush_every(std::chrono::seconds(3));

        if (spdlog::get("goodnet")) spdlog::drop("goodnet");
        spdlog::register_logger(logger);

        get_instance() = logger;

        logger->info("Logger initialized. level={} mode={} root='{}'",
                     log_level_, source_detail_mode_,
                     project_root_.empty() ? "(basename only)" : project_root_);

    } catch (const spdlog::spdlog_ex& ex) {
        std::cerr << "[Logger] init failed: " << ex.what() << '\n';
        throw;
    }
}

// ─── shutdown ───────────────────────────────────────────────────────────────

void Logger::shutdown() {
    auto& inst = get_instance();
    if (!inst) return;
    inst->info("Logger shutting down");
    inst->flush();
    spdlog::drop_all();
    inst.reset();
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
