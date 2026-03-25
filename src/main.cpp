/// @file src/main.cpp
/// @brief GoodNet CLI entry point — orchestration only.

#include <atomic>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>

#include <boost/program_options.hpp>
#include <sodium.h>

#include "core.hpp"
#include "config.hpp"
#include "pluginManager.hpp"

#include "bench.hpp"
#include "server.hpp"
#include "dashboard.hpp"

namespace po = boost::program_options;

// ─── Signal handling ─────────────────────────────────────────────────────────

static std::atomic<bool> g_keep_running{true};

static void signal_handler(int) {
    g_keep_running.store(false, std::memory_order_relaxed);
}

static void setup_signals() {
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);
#if !defined(_WIN32)
    std::signal(SIGPIPE, SIG_IGN);
#endif
}

// ─── main ────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    if (sodium_init() < 0) {
        std::cerr << "[FATAL] libsodium init failed\n";
        return 1;
    }
    setup_signals();

    // ── CLI flags ────────────────────────────────────────────────────────────
    std::string target;
    uint16_t listen_port = 0;
    int      threads     = 0;
    uint64_t pkts_limit  = 0;
    size_t   kb_size     = 64;
    double   report_hz   = 2.0;
    double   timeout     = 30.0;
    double   exit_after  = 0.0;
    bool     no_color    = false;
    bool     exit_code   = false;
    bool     ice_upgrade = false;
    std::string config_path;

    po::options_description desc("GoodNet Benchmark");
    desc.add_options()
        ("help,h",       "Show help")
        ("target,t",     po::value(&target),          "Target URI (tcp://IP:PORT)")
        ("listen,l",     po::value(&listen_port),     "Server listen port")
        ("threads,j",    po::value(&threads)->default_value(0),
                         "IO+worker threads (0=auto)")
        ("count,n",      po::value(&pkts_limit)->default_value(0),
                         "Packet limit (0=unlimited)")
        ("size,s",       po::value(&kb_size)->default_value(64),
                         "Packet size in KB")
        ("hz",           po::value(&report_hz)->default_value(2.0),
                         "Dashboard refresh rate (Hz)")
        ("timeout",      po::value(&timeout)->default_value(30.0),
                         "Connection/handshake timeout in seconds")
        ("no-color",     po::bool_switch(&no_color),  "Disable ANSI colors")
        ("exit-after",   po::value(&exit_after)->default_value(0),
                         "Exit after N seconds (for CI; 0=disabled)")
        ("exit-code",    po::bool_switch(&exit_code),
                         "Structured exit codes: 0=ok, 1=crypto/timeout error")
        ("ice-upgrade",  po::bool_switch(&ice_upgrade),
                         "Upgrade to ICE/DTLS after TCP handshake")
        ("config,c",     po::value(&config_path),     "Path to JSON config file");

    po::variables_map vm;
    try {
        po::store(po::parse_command_line(argc, argv, desc), vm);
        po::notify(vm);
    } catch (const std::exception& e) {
        std::cerr << "Argument error: " << e.what() << "\n";
        return 1;
    }
    if (vm.count("help")) { std::cout << desc << "\n"; return 0; }

    // ── Thread count ─────────────────────────────────────────────────────────
    if (threads <= 0) {
        const int hw = static_cast<int>(std::thread::hardware_concurrency());
        threads = (hw > 0) ? hw : 2;
    }

    // ── Core ─────────────────────────────────────────────────────────────────
    Config config(true);
    config.core.io_threads   = std::max(1, threads);
    config.plugins.auto_load = true;

    if (!config_path.empty())
        config.load_from_file(config_path);

    if (const char* env_dir = std::getenv("GOODNET_PLUGINS_DIR")) {
        config.plugins.base_dir = env_dir;
    } else {
        config.plugins.base_dir  = "./result/plugins";
        config.plugins.extra_dirs = "./plugins";
    }

    std::printf(">>> Threads: %d  |  Packet: %zu KB  |  Target: %s\n",
                threads, kb_size,
                target.empty() ? "(server only)" : target.c_str());

    gn::Core core(&config);
    core.run_async();

    // ── Server listen ────────────────────────────────────────────────────────
    if (listen_port > 0) {
        if (auto opt = core.pm().find_connector_by_scheme("tcp")) {
            (*opt)->listen((*opt)->connector_ctx, "0.0.0.0", listen_port);
            std::printf(">>> [Server] Listening on 0.0.0.0:%u\n", listen_port);
        } else {
            std::fprintf(stderr, "!!! TCP Connector plugin NOT found in: %s\n",
                         config.plugins.base_dir.c_str());
            core.stop();
            return 1;
        }
    }

    // ── Dispatch to bench or server mode ─────────────────────────────────────
    int final_exit = 0;

    if (!target.empty()) {
        cli::BenchConfig bcfg;
        bcfg.target      = target;
        bcfg.threads     = threads;
        bcfg.pkt_limit   = pkts_limit;
        bcfg.pkt_size_kb = kb_size;
        bcfg.report_hz   = report_hz;
        bcfg.timeout     = timeout;
        bcfg.no_color    = no_color;
        bcfg.ice_upgrade = ice_upgrade;

        auto result = cli::run_benchmark(core, bcfg, g_keep_running);
        cli::print_summary(result.throughput, result.total_sec,
                           result.total_bytes, result.total_pkts, result.stats);

        if (result.exit_status != 0) final_exit = result.exit_status;

        if (exit_code && final_exit == 0) {
            if (result.stats.auth_fail > 0 || result.stats.decrypt_fail > 0)
                final_exit = 1;
        }
    } else {
        cli::ServerConfig scfg;
        scfg.report_hz  = report_hz;
        scfg.exit_after = exit_after;
        scfg.no_color   = no_color;

        auto result = cli::run_server(core, scfg, g_keep_running);
        cli::print_summary(result.throughput, result.total_sec,
                           0, 0, result.stats);

        if (exit_code) {
            if (result.stats.auth_fail > 0 || result.stats.decrypt_fail > 0)
                final_exit = 1;
        }
    }

    // ── Shutdown ─────────────────────────────────────────────────────────────
    std::printf("\n>>> Shutting down core...\n");
    core.stop();
    std::printf(">>> Done.\n");

    if (final_exit != 0) {
        std::fprintf(stderr, "!!! Exit code %d\n", final_exit);
    }
    return final_exit;
}
