#include <filesystem>
#include <vector>
#include <thread>

#include "pluginManager.hpp"
#include "logger.hpp"
#include "config.hpp"
#include "signals.hpp"

namespace fs = std::filesystem;

int main() {
    boost::asio::io_context ioc;
    auto work_guard = boost::asio::make_work_guard(ioc);

    // ── 1. Config ─────────────────────────────────────────────────────────────
    Config conf;

    // ── 2. Logger из конфига ──────────────────────────────────────────────────
    // Выставляем ДО первого LOG_* — подхватятся при ленивой инициализации.
    Logger::log_level = conf.get_or<std::string>("logging.level", "info");
    Logger::log_file  = conf.get_or<std::string>("logging.file",  "logs/goodnet.log");
    Logger::max_size  = static_cast<size_t>(conf.get_or<int>("logging.max_size", 10 * 1024 * 1024));
    Logger::max_files = conf.get_or<int>("logging.max_files", 5);

    LOG_INFO("Booting GoodNet core...");

    // ── 3. Host API ───────────────────────────────────────────────────────────
    // internal_logger: сырой указатель на spdlog::logger ядра.
    // Плагины через sync_plugin_context(api) оборачивают его в shared_ptr
    // с no-op deleter — используют объект без претензии на владение.
    // Жизненный цикл: Logger::logger_ гарантированно живёт до Logger::shutdown().
    host_api_t api{};
    api.internal_logger = static_cast<void*>(Logger::get().get());

    // ── 4. Путь к плагинам ───────────────────────────────────────────────────
    // Приоритет: config > GOODNET_PLUGINS_DIR > ./plugins
    fs::path plugins_path = conf.get_or<std::string>("plugins.base_dir", "");
    if (plugins_path.empty()) {
        const char* env = std::getenv("GOODNET_PLUGINS_DIR");
        plugins_path = env ? env : "./plugins";
    }

    // ── 5. PluginManager ──────────────────────────────────────────────────────
    gn::PluginManager manager(&api, plugins_path);
    manager.load_all_plugins();

    // ── 6. Роутинг через сигнал ───────────────────────────────────────────────
    gn::PacketSignal packet_signal(ioc);

    packet_signal.connect([&manager](auto header, auto endpoint, auto data) {
        const auto handlers = manager.get_active_handlers();
        for (handler_t* plugin : handlers) {
            if (!plugin->handle_message) continue;

            // Нулевой тип или явное совпадение = плагин слушает это сообщение
            bool accepted = (plugin->num_supported_types == 0);
            for (size_t i = 0; !accepted && i < plugin->num_supported_types; ++i)
                accepted = (plugin->supported_types[i] == 0 ||
                            plugin->supported_types[i] == header->payload_type);

            if (accepted)
                plugin->handle_message(plugin->user_data,
                                       header.get(), endpoint,
                                       data->data(), data->size());
        }
    });

    // ── 7. IO thread pool ─────────────────────────────────────────────────────
    std::vector<std::thread> pool;
    pool.reserve(12);
    for (int i = 0; i < 12; ++i)
        pool.emplace_back([&ioc] { ioc.run(); });

    // ── 8. Blast test ─────────────────────────────────────────────────────────
    LOG_INFO("Starting blast: 100,000 messages");
    for (int i = 0; i < 100'000; ++i) {
        auto hdr          = std::make_shared<header_t>();
        hdr->magic        = GNET_MAGIC;
        hdr->packet_id    = static_cast<uint64_t>(i);
        hdr->payload_type = 0;
        auto payload      = std::make_shared<std::vector<char>>(128, 'G');
        hdr->payload_len  = static_cast<uint32_t>(payload->size());
        packet_signal.emit(hdr, nullptr, payload);
    }

    work_guard.reset();
    for (auto& t : pool)
        if (t.joinable()) t.join();

    LOG_INFO("All messages processed. Cleaning up...");

    // ── 9. Завершение ─────────────────────────────────────────────────────────
    // Порядок КРИТИЧЕН:
    //   a) unload_all() → плагины получают shutdown(), LOG_* в on_shutdown() работают
    //      (Logger::logger_ ещё жив, set_external_logger shared_ptr с no-op deleter тоже)
    //   b) Logger::shutdown() → flush + spdlog::drop_all() + logger_.reset()
    //      После reset() Logger::logger_ = nullptr — деструктор shared_ptr больше
    //      не вызывает _M_dispose, объект уже не принадлежит нашему shared_ptr.
    //
    // НЕ вызывать spdlog::shutdown() напрямую:
    //   signals.hpp → boost/asio.hpp → sys/socket.h → ::shutdown() (POSIX)
    //   GCC видит ambiguity: spdlog::shutdown vs ::shutdown → ошибка компиляции.
    //   Семантически тоже неверно: spdlog::shutdown() убивает internal threadpool
    //   до того как Logger::logger_ shared_ptr корректно обнуляется → SIGSEGV.
    manager.unload_all();
    Logger::shutdown();

    return 0;
}
