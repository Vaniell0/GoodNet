#include <iostream>
#include <filesystem>
#include <cstdlib>
#include "pluginManager.hpp"
#include "logger.hpp"
#include "config.hpp"

namespace fs = std::filesystem;

// ─── Host API заглушки ────────────────────────────────────────────────────────

static void host_send_impl(const char* uri,
                            [[maybe_unused]] uint32_t type,
                            [[maybe_unused]] const void* data,
                            size_t size) {
    LOG_INFO("[send] → {} | {} bytes", uri, size);
}

// ─── main ────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    // ── 1. Config ─────────────────────────────────────────────────────────────
    Config conf;

    // ── 2. Logger из конфига ──────────────────────────────────────────────────
    // Выставляем ДО первого LOG_* — подхватятся в init_internal().
    Logger::log_level          = conf.get_or<std::string>("logging.level",   "info");
    Logger::log_file           = conf.get_or<std::string>("logging.file",    "logs/goodnet.log");
    Logger::max_size           = static_cast<size_t>(
                                   conf.get_or<int>("logging.max_size", 10 * 1024 * 1024));
    Logger::max_files          = conf.get_or<int>("logging.max_files",  5);
    Logger::source_detail_mode = conf.get_or<int>("logging.source_detail_mode", 0);
    Logger::get()->info("Booting GoodNet core...");

    // ── 3. Host API ──────────────────────────────────────────────────────────
    host_api_t api{};
    api.send = host_send_impl;
    api.internal_logger = (void*)Logger::get().get();

    // ── 4. Путь к плагинам ───────────────────────────────────────────────────
    // Приоритет: env GOODNET_PLUGINS_DIR > config > ./plugins
    // GOODNET_PLUGINS_DIR задаётся через wrapProgram в flake.nix для nix builds.
    fs::path plugins_path;

    if (const char* env = std::getenv("GOODNET_PLUGINS_DIR")) {
        plugins_path = env;
        LOG_INFO("Plugins path from GOODNET_PLUGINS_DIR: {}", plugins_path.string());
    } else {
        plugins_path = conf.get_or<fs::path>("plugins.base_dir",
                           fs::current_path() / "plugins");
        if (plugins_path.is_relative())
            plugins_path = fs::absolute(fs::current_path() / plugins_path).lexically_normal();
    }

    // ── 5. PluginManager ──────────────────────────────────────────────────────
    gn::PluginManager manager(&api, plugins_path);

    LOG_INFO("=== GoodNet Plugin System Test ===");
    LOG_INFO("Plugins directory: {}", plugins_path.string());

    if (argc < 2) {
        manager.load_all_plugins();
    } else {
        // Ручная загрузка одного плагина: ./goodnet path/to/plugin.so
        fs::path plugin_path = argv[1];
        auto result = manager.load_plugin(plugin_path);
        if (!result) {
            LOG_ERROR("Failed to load '{}': {}",
                      plugin_path.filename().string(), result.error());
            return 1;
        }
    }

    manager.list_plugins();

    LOG_INFO("Exiting. All plugins will be unloaded by RAII.");
    return 0;
}
