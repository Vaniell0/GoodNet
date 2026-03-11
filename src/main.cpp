#include <iostream>
#include <chrono>
#include <atomic>
#include <vector>
#include <thread>
#include <csignal>
#include <iomanip>
#include <boost/program_options.hpp>
#include <sodium.h>

#include "core.hpp"
#include "connectionManager.hpp"
#include "pluginManager.hpp"

namespace po = boost::program_options;
using Clock = std::chrono::steady_clock;

std::atomic<bool> g_keep_running{true};
void signal_handler(int) { g_keep_running = false; }

int main(int argc, char** argv) {
    if (sodium_init() < 0) return 1;
    std::signal(SIGINT, signal_handler);

    std::string target;
    uint16_t listen_port = 0;
    int threads = std::thread::hardware_concurrency();
    uint64_t pkts_limit = 0;
    size_t kb_size = 64;

    po::options_description desc("GoodNet High-Performance Benchmark");
    desc.add_options()
        ("help,h", "Показать помощь")
        ("target,t", po::value<std::string>(&target), "Цель для стресс-теста (tcp://IP:PORT)")
        ("listen,l", po::value<uint16_t>(&listen_port), "Порт для прослушивания (режим сервера)")
        ("threads,j", po::value<int>(&threads)->default_value(threads), "Количество потоков")
        ("count,n", po::value<uint64_t>(&pkts_limit)->default_value(0), "Лимит пакетов (0 = бесконечно)")
        ("size,s", po::value<size_t>(&kb_size)->default_value(64), "Размер пакета в КБ");

    po::variables_map vm;
    try {
        po::store(po::parse_command_line(argc, argv, desc), vm);
        po::notify(vm);
    } catch (const std::exception& e) {
        std::cerr << "Ошибка аргументов: " << e.what() << std::endl;
        return 1;
    }

    if (vm.count("help")) {
        std::cout << desc << std::endl;
        return 0;
    }

    gn::CoreConfig cfg;
    cfg.network.io_threads = std::max(1, threads);
    cfg.plugins.auto_load = true;
    
    if (const char* env_dir = std::getenv("GOODNET_PLUGINS_DIR")) {
        cfg.plugins.dirs = { env_dir };
    } else {
        cfg.plugins.dirs = { "./result/plugins", "./plugins" };
    }

    gn::Core core(cfg);
    core.run_async();

    // --- Режим сервера ---
    if (listen_port > 0) {
        if (auto opt = core.pm().find_connector_by_scheme("tcp")) {
            (*opt)->listen((*opt)->connector_ctx, "0.0.0.0", listen_port);
            std::cout << ">>> [Server] Listening on 0.0.0.0:" << listen_port << std::endl;
        } else {
            // Если плагина нет, громко ругаемся и падаем!
            std::cerr << "!!! [ERROR] TCP Connector plugin NOT found! Check plugins dir: " 
                      << cfg.plugins.dirs[0].string() << std::endl;
            return 1;
        }
    }

    // --- Режим клиента ---
    if (!target.empty()) {
        std::cout << ">>> [Client] Waiting for link: " << target << "..." << std::endl;
        
        // Цикл ожидания рукопожатия
        bool connected = false;
        while (g_keep_running) {
            core.send(target.c_str(), 0, nullptr, 0); // Пингуем хэндшейком
            
            auto uris = core.active_uris();
            for(const auto& u : uris) {
                if(u.find(target) != std::string::npos || target.find(u) != std::string::npos) {
                    connected = true;
                    break;
                }
            }
            if (connected) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        if (connected) {
            std::cout << ">>> [Client] Connected! Firing " << threads << " workers." << std::endl;
            
            std::atomic<uint64_t> total_bytes{0};
            std::atomic<uint64_t> sent_pkts{0};
            auto t_start = Clock::now();

            std::vector<std::thread> workers;
            for (int i = 0; i < threads; ++i) {
                workers.emplace_back([&, kb_size, target, pkts_limit]() {
                    std::vector<uint8_t> payload(kb_size * 1024);
                    randombytes_buf(payload.data(), payload.size());
                    
                    while (g_keep_running) {
                        // Проверка лимита пакетов
                        if (pkts_limit > 0 && sent_pkts.load(std::memory_order_relaxed) >= pkts_limit) break;

                        // Защита от переполнения очереди (Backpressure)
                        if (core.cm().get_pending_bytes() > 128 * 1024 * 1024) { // 128MB max
                            std::this_thread::yield(); 
                            continue;
                        }

                        core.send(target.c_str(), 100, payload.data(), payload.size());
                        
                        total_bytes.fetch_add(payload.size(), std::memory_order_relaxed);
                        sent_pkts.fetch_add(1, std::memory_order_relaxed);
                    }
                });
            }

            // Мониторинг
            auto last_report = Clock::now();
            uint64_t last_bytes = 0;

            while (g_keep_running) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                
                auto now = Clock::now();
                double duration = std::chrono::duration<double>(now - last_report).count();
                uint64_t current_bytes = total_bytes.load();
                uint64_t delta_bytes = current_bytes - last_bytes;
                
                double bps = (delta_bytes * 8.0) / duration;
                double total_sec = std::chrono::duration<double>(now - t_start).count();

                std::printf("\r[Bench] %6.2f Gbps | %7.0f pkt/s | Sent: %8.1f MB | Backlog: %5.1f MB  ", 
                            bps / 1e9, 
                            (sent_pkts.load() / total_sec),
                            current_bytes / 1e6,
                            core.cm().get_pending_bytes() / 1e6);
                std::fflush(stdout);

                last_report = now;
                last_bytes = current_bytes;

                if (pkts_limit > 0 && sent_pkts.load() >= pkts_limit) break;
            }

            for (auto& w : workers) {
                if(w.joinable()) w.join();
            }
        }
    } else {
        // Просто режим сервера: ждем Ctrl+C
        while(g_keep_running) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "\n>>> Shutting down core..." << std::endl;
    core.stop(); 
    return 0;
}