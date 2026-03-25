/// @file apps/store/main.cpp
/// @brief GoodNet Store — минимальный шаблон распределённого хранилища.
///
/// Использование:
///   goodnet_store --listen 0.0.0.0 --port 25566 --db store.db
///   goodnet_store --seed tcp://peer1:25565 --seed tcp://peer2:25565

#include "store_app.hpp"

#include <boost/program_options.hpp>
#include <sodium.h>

#include <atomic>
#include <csignal>
#include <iostream>

namespace po = boost::program_options;

static std::atomic<bool> g_running{true};

static void signal_handler(int) {
    g_running.store(false, std::memory_order_relaxed);
}

int main(int argc, char** argv) {
    if (sodium_init() < 0) {
        std::cerr << "[FATAL] libsodium init failed\n";
        return 1;
    }

    // CLI
    gn::store::StoreConfig cfg;
    std::vector<std::string> seeds;

    po::options_description desc("GoodNet Store");
    desc.add_options()
        ("help,h",    "Show help")
        ("listen",    po::value<std::string>(&cfg.listen_address)
                          ->default_value("0.0.0.0"),
                      "Listen address")
        ("port,p",    po::value<uint16_t>(&cfg.listen_port)
                          ->default_value(25566),
                      "Listen port")
        ("db",        po::value<std::string>(&cfg.db_path)
                          ->default_value("store.db"),
                      "SQLite database path")
        ("backend",   po::value<std::string>(&cfg.backend_type)
                          ->default_value("sqlite"),
                      "Backend type (sqlite)")
        ("cleanup",   po::value<uint32_t>(&cfg.cleanup_interval_s)
                          ->default_value(600),
                      "Cleanup interval in seconds")
        ("seed",      po::value<std::vector<std::string>>(&seeds)->multitoken(),
                      "Seed peer URIs (tcp://host:port)");

    po::variables_map vm;
    try {
        po::store(po::parse_command_line(argc, argv, desc), vm);
        po::notify(vm);
    } catch (const std::exception& e) {
        std::cerr << "Argument error: " << e.what() << "\n";
        return 1;
    }

    if (vm.count("help")) {
        std::cout << desc << "\n";
        return 0;
    }

    cfg.seed_peers = std::move(seeds);

    // Сигналы
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);
#if !defined(_WIN32)
    std::signal(SIGPIPE, SIG_IGN);
#endif

    try {
        gn::store::StoreApp app(cfg);

        // Запустить в фоне, ждать сигнала
        std::thread app_thread([&]() { app.run(); });

        while (g_running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        app.stop();
        if (app_thread.joinable())
            app_thread.join();

    } catch (const std::exception& e) {
        std::cerr << "[FATAL] " << e.what() << "\n";
        return 1;
    }

    return 0;
}
