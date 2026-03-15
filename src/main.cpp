/// @file src/main.cpp
/// @brief GoodNet benchmark / server entry point.

#include <iostream>
#include <iomanip>
#include <sstream>
#include <chrono>
#include <atomic>
#include <vector>
#include <thread>
#include <csignal>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <numeric>
#include <array>

#include <boost/program_options.hpp>
#include <sodium.h>

#include "core.hpp"
#include "connectionManager.hpp"
#include "pluginManager.hpp"

namespace po = boost::program_options;
using Clock     = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Seconds   = std::chrono::duration<double>;

// ─── Signal handling ──────────────────────────────────────────────────────────

static std::atomic<bool> g_keep_running{true};

static void signal_handler(int) {
    g_keep_running.store(false, std::memory_order_relaxed);
}

static void setup_signals() {
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);
#if !defined(_WIN32)
    std::signal(SIGPIPE, SIG_IGN);   // Ignore broken TCP pipe.
#endif
}

// ─── Hex helper ──────────────────────────────────────────────────────────────

static std::string local_bytes_to_hex(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        char b[3];
        std::snprintf(b, 3, "%02x", data[i]);
        out += b;
    }
    return out;
}

// ─── Throughput sample ring ───────────────────────────────────────────────────
/// Keeps the last N throughput samples (Gbps) for percentile computation.

static constexpr size_t SAMPLE_RING = 1200;   ///< 10 min at 0.5s interval.

struct Samples {
    std::array<double, SAMPLE_RING> data{};
    size_t head = 0;
    size_t count = 0;

    void push(double v) {
        data[head] = v;
        head = (head + 1) % SAMPLE_RING;
        if (count < SAMPLE_RING) ++count;
    }

    /// Returns a sorted copy of accumulated values.
    std::vector<double> sorted() const {
        std::vector<double> v(data.begin(), data.begin() + count);
        std::sort(v.begin(), v.end());
        return v;
    }

    double percentile(double p) const {
        auto s = sorted();
        if (s.empty()) return 0.0;
        const size_t idx = static_cast<size_t>(p / 100.0 * (s.size() - 1));
        return s[std::min(idx, s.size() - 1)];
    }

    double mean() const {
        if (count == 0) return 0.0;
        double sum = 0;
        for (size_t i = 0; i < count; ++i) sum += data[i];
        return sum / count;
    }

    double peak() const {
        if (count == 0) return 0.0;
        return *std::max_element(data.begin(), data.begin() + count);
    }
};

// ─── Formatting helpers ───────────────────────────────────────────────────────

static std::string fmt_bytes(double bytes) {
    if (bytes < 1e3)  return std::to_string(static_cast<int>(bytes)) + " B";
    if (bytes < 1e6)  { char b[32]; std::snprintf(b, sizeof(b), "%.1f KB", bytes / 1e3); return b; }
    if (bytes < 1e9)  { char b[32]; std::snprintf(b, sizeof(b), "%.1f MB", bytes / 1e6); return b; }
                      { char b[32]; std::snprintf(b, sizeof(b), "%.2f GB", bytes / 1e9); return b; }
}

static std::string fmt_duration(double sec) {
    const int h = static_cast<int>(sec) / 3600;
    const int m = (static_cast<int>(sec) % 3600) / 60;
    const int s = static_cast<int>(sec) % 60;
    char buf[32];
    if (h > 0) std::snprintf(buf, sizeof(buf), "%dh %02dm %02ds", h, m, s);
    else       std::snprintf(buf, sizeof(buf), "%dm %02ds", m, s);
    return buf;
}

static std::string bar(double fraction, int width = 20) {
    const int filled = static_cast<int>(std::clamp(fraction, 0.0, 1.0) * width);
    std::string b;
    b.reserve(width * 3); // Reserve space for UTF-8 block characters (3 bytes each).
    for (int i = 0; i < filled; ++i) b += "█";
    for (int i = filled; i < width; ++i) b += "░";
    return b;
}
// ─── Live dashboard ───────────────────────────────────────────────────────────

static void print_live(double gbps, double gbps_peak,
                        double pkt_s, double total_sec,
                        uint64_t total_bytes, uint64_t total_pkts,
                        double backlog_mb,
                        const gn::StatsSnapshot& st) {

    // Single-line dashboard, overwritten via \r each tick.
    // (full TUI would require ftxui — this is a benchmark, not a CLI).

    std::printf(
        "\033[2K\r"  // очистить строку
        "[%s] "
        "\033[1;36m%.2f Gbps\033[0m (peak \033[33m%.2f\033[0m) | "
        "\033[1;32m%7.0f pkt/s\033[0m | "
        "Sent \033[1m%s\033[0m | "
        "Backlog \033[%sm%.1f MB\033[0m | "
        "RX %s | TX %s | "
        "Auth ✓%llu ✗%llu | "
        "Drops %llu",
        fmt_duration(total_sec).c_str(),
        gbps,
        gbps_peak,
        pkt_s,
        fmt_bytes(static_cast<double>(total_bytes)).c_str(),
        (backlog_mb > 64) ? "1;31" : "0",   // красный если > 64MB backlog
        backlog_mb,
        fmt_bytes(static_cast<double>(st.rx_bytes)).c_str(),
        fmt_bytes(static_cast<double>(st.tx_bytes)).c_str(),
        (unsigned long long)st.auth_ok,
        (unsigned long long)st.auth_fail,
        (unsigned long long)st.backpressure
    );
    std::fflush(stdout);
}

// ─── Final summary ────────────────────────────────────────────────────────────

static std::string fmt_num(uint64_t n) {
    std::string s = std::to_string(n);
    int insert_at = static_cast<int>(s.length()) - 3;
    while (insert_at > 0) {
        s.insert(insert_at, ",");
        insert_at -= 3;
    }
    return s;
}

/// Counts visible characters in a UTF-8 string (skipping continuation bytes).
static int utf8_len(const std::string& str) {
    int len = 0;
    for (size_t i = 0; i < str.length(); ++i) {
        if ((str[i] & 0xc0) != 0x80) len++;
    }
    return len;
}

static void print_row(const char* label, const std::string& value, const char* color = "") {
    const int total_width = 58; // Inner width of the box frame.
    const std::string reset = "\033[0m";

    // Build left part: "  Label : "
    std::string prefix = "  ";
    prefix += label;
    prefix += " : ";

    int prefix_vis_len = utf8_len(prefix);
    int value_vis_len = utf8_len(value);

    std::printf("║%s%s%s", prefix.c_str(), color, value.c_str());
    std::printf("%s", reset.c_str());

    // Pad with spaces: total_width - prefix_len - value_len.
    int padding = total_width - prefix_vis_len - value_vis_len;
    if (padding > 0) {
        for (int i = 0; i < padding; ++i) std::printf(" ");
    }
    std::printf("║\n");
}

static void print_summary(const Samples& thr_samples,
                           double total_sec,
                           uint64_t total_bytes,
                           uint64_t total_pkts,
                           const gn::StatsSnapshot& st) {

    const bool has_errors = st.decrypt_fail > 0 || st.auth_fail > 0;
    const char* c_red   = "\033[1;31m";
    const char* c_green = "\033[1;32m";
    const char* c_none  = "";

    std::printf("\n\n");
    std::printf("╔══════════════════════════════════════════════════════════╗\n");
    std::printf("║                GoodNet Benchmark — ИТОГИ                 ║\n");
    std::printf("╠══════════════════════════════════════════════════════════╣\n");

    print_row("Длительность", fmt_duration(total_sec));
    print_row("Статус", has_errors ? "ЗАВЕРШЕНО (ошибки!)" : "ЗАВЕРШЕНО (OK)", has_errors ? c_red : c_green);

    // --- Пропускная способность ---
    const double avg_gbps = thr_samples.mean();
    const double peak     = thr_samples.peak();
    
    std::printf("╠══════════════════ ТРАФИК (Throughput) ═══════════════════╣\n");
    
    // Manual string assembly for avg — bar() breaks printf alignment.
    char avg_buf[128];
    std::snprintf(avg_buf, sizeof(avg_buf), "%.2f Gbps  %s", avg_gbps, bar(avg_gbps / (peak > 0 ? peak : 1), 20).c_str());
    print_row("Среднее", avg_buf);
    
    print_row("Пик", (std::ostringstream() << std::fixed << std::setprecision(2) << peak << " Gbps").str());
    print_row("Задержки (p95)", (std::ostringstream() << std::fixed << std::setprecision(2) << thr_samples.percentile(95) << " Gbps").str());

    // --- L7 Data ---
    std::printf("╠══════════════════ ПЕРЕДАЧА (L7 Data) ════════════════════╣\n");
    uint64_t display_bytes = (total_bytes > 0) ? total_bytes : st.rx_bytes;
    uint64_t display_pkts  = (total_pkts > 0) ? total_pkts : st.rx_packets;

    print_row("Всего данных", fmt_bytes(static_cast<double>(display_bytes)));
    print_row("Всего пакетов", fmt_num(display_pkts));
    
    const double pkt_s_avg = total_sec > 0 ? display_pkts / total_sec : 0;
    print_row("Темп (Avg pkt/s)", fmt_num(static_cast<uint64_t>(pkt_s_avg)));

    // --- Core state ---
    std::printf("╠══════════════════ СОСТОЯНИЕ ЯДРА (Core) ═════════════════╣\n");
    
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%u / %u", (unsigned)st.total_conn, (unsigned)st.total_disc);
    print_row("Соединения (OK/Err)", buf);
    
    std::snprintf(buf, sizeof(buf), "%llu / %llu", (unsigned long long)st.auth_ok, (unsigned long long)st.auth_fail);
    print_row("Авторизация (✓/✗)", buf);

    uint64_t df = st.decrypt_fail;
    print_row("Ошибки дешифрации", fmt_num(df), df > 0 ? c_red : c_none);
    print_row("Потери (Backpres.)", fmt_num(st.backpressure));

    // --- Histogram ---
    if (thr_samples.count >= 4) {
        std::printf("╠══════════════════ Histogram Gbps ════════════════════════╣\n");
        auto sv = thr_samples.sorted();
        const double lo = sv.front(), hi = sv.back();
        const int BINS = 8;
        const int BAR_MAX_WIDTH = 24; // Max visual bar width in characters.
        
        std::array<int, BINS> bins{};
        for (double v : sv) {
            const int b = hi > lo ? static_cast<int>((v - lo) / (hi - lo) * (BINS - 1)) : 0;
            ++bins[std::min(b, BINS - 1)];
        }

        for (int i = 0; i < BINS; ++i) {
            // Number of filled block characters to draw.
            int num_blocks = (int)(bins[i] * BAR_MAX_WIDTH / (sv.empty() ? 1 : sv.size()));
            
            std::string hb;
            for(int j=0; j < num_blocks; ++j) hb += "█";
            
            // Pad with spaces for visual alignment.
            std::string spaces;
            for(int j=0; j < (BAR_MAX_WIDTH - num_blocks); ++j) spaces += " ";
            
            char label[64];
            double b_start = lo + (hi - lo) * i / BINS;
            double b_end   = lo + (hi - lo) * (i + 1) / BINS;
            std::snprintf(label, sizeof(label), "%5.1f-%-5.1f Gbps", b_start, b_end);
            
            // Print: label | bars + padding | percentage.
            std::printf("║  %s │ %s%s│ %3d%%  ║\n",
                label, hb.c_str(), spaces.c_str(), (int)(100.0 * bins[i] / sv.size()));
        }
    }

    std::printf("╚══════════════════════════════════════════════════════════╝\n");
}

// ─── main ─────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    if (sodium_init() < 0) {
        std::cerr << "[FATAL] libsodium init failed\n";
        return 1;
    }
    setup_signals();

    // ── CLI ──────────────────────────────────────────────────────────────────
    std::string target;
    uint16_t listen_port   = 0;
    int      threads       = 0;          // 0 = auto
    uint64_t pkts_limit    = 0;
    size_t   kb_size       = 64;
    double   report_hz     = 2.0;        // Dashboard refresh rate (Hz).
    bool     no_color      = false;
    double   exit_after    = 0.0;        // 0 = disabled (server runs forever)
    bool     exit_code     = false;      // structured exit codes for CI
    bool     ice_upgrade   = false;      // ICE/DTLS upgrade after TCP handshake
    std::string config_path;

    po::options_description desc("GoodNet Benchmark");
    desc.add_options()
        ("help,h",    "Show help")
        ("target,t",  po::value<std::string>(&target),
                      "Target URI (tcp://IP:PORT)")
        ("listen,l",  po::value<uint16_t>(&listen_port),
                      "Server listen port")
        ("threads,j", po::value<int>(&threads)->default_value(0),
                      "IO+worker threads (0=auto)")
        ("count,n",   po::value<uint64_t>(&pkts_limit)->default_value(0),
                      "Packet limit (0=unlimited)")
        ("size,s",    po::value<size_t>(&kb_size)->default_value(64),
                      "Packet size in KB")
        ("hz",        po::value<double>(&report_hz)->default_value(2.0),
                      "Dashboard refresh rate (Hz)")
        ("no-color",  po::bool_switch(&no_color),
                      "Disable ANSI colors")
        ("exit-after", po::value<double>(&exit_after)->default_value(0),
                      "Exit after N seconds (for CI; 0=disabled)")
        ("exit-code", po::bool_switch(&exit_code),
                      "Structured exit codes: 0=ok, 1=crypto error")
        ("ice-upgrade", po::bool_switch(&ice_upgrade),
                      "Upgrade to ICE/DTLS after TCP handshake (NAT traversal)")
        ("config,c",  po::value<std::string>(&config_path),
                      "Path to JSON config file");

    po::variables_map vm;
    try {
        po::store(po::parse_command_line(argc, argv, desc), vm);
        po::notify(vm);
    } catch (const std::exception& e) {
        std::cerr << "Argument error: " << e.what() << "\n";
        return 1;
    }
    if (vm.count("help")) { std::cout << desc << "\n"; return 0; }

    // ── Auto-detect thread count ──────────────────────────────────────────────
    if (threads <= 0) {
        const int hw = static_cast<int>(std::thread::hardware_concurrency());
        threads = (hw > 0) ? hw : 2;
    }

    // ── Core ─────────────────────────────────────────────────────────────────
    gn::CoreConfig cfg;
    cfg.network.io_threads = std::max(1, threads);
    cfg.plugins.auto_load  = true;

    if (!config_path.empty())
        cfg.config_file = config_path;

    if (const char* env_dir = std::getenv("GOODNET_PLUGINS_DIR")) {
        cfg.plugins.dirs = { env_dir };
    } else {
        cfg.plugins.dirs = { "./result/plugins", "./plugins" };
    }

    std::printf(">>> Threads: %d  |  Packet: %zu KB  |  Target: %s\n",
                threads, kb_size, target.empty() ? "(server only)" : target.c_str());

    gn::Core core(cfg);
    core.run_async();

    // ── Server ───────────────────────────────────────────────────────────────
    if (listen_port > 0) {
        if (auto opt = core.pm().find_connector_by_scheme("tcp")) {
            (*opt)->listen((*opt)->connector_ctx, "0.0.0.0", listen_port);
            std::printf(">>> [Server] Listening on 0.0.0.0:%u\n", listen_port);
        } else {
            std::fprintf(stderr, "!!! TCP Connector plugin NOT found in: %s\n",
                         cfg.plugins.dirs[0].string().c_str());
            return 1;
        }
    }

    // ── Client ───────────────────────────────────────────────────────────────
    std::atomic<uint64_t> total_bytes_sent{0};
    std::atomic<uint64_t> total_pkts_sent {0};
    TimePoint             t_start;
    Samples               thr_samples;

    if (!target.empty()) {
        std::printf(">>> [Client] Waiting for handshake: %s ...\n", target.c_str());

        // Wait for handshake completion.
        while (g_keep_running) {
            core.send(target.c_str(), 0, nullptr, 0);
            bool found = false;
            for (const auto& u : core.active_uris())
                if (u.find(target) != std::string::npos ||
                    target.find(u)  != std::string::npos)
                    { found = true; break; }
            if (found) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (!g_keep_running) goto shutdown;

        // ── ICE upgrade ──────────────────────────────────────────────────────
        if (ice_upgrade) {
            std::string peer_hex;
            for (auto cid : core.active_conn_ids()) {
                auto pk = core.peer_pubkey(cid);
                if (!pk.empty()) {
                    peer_hex = local_bytes_to_hex(pk.data(), pk.size());
                    break;
                }
            }
            if (!peer_hex.empty()) {
                std::printf(">>> [Client] ICE upgrade: negotiating...\n");
                core.connect("ice://" + peer_hex);

                auto ice_start = Clock::now();
                while (g_keep_running && core.connection_count() < 2) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    if (Seconds(Clock::now() - ice_start).count() > 30.0) {
                        std::fprintf(stderr, "!!! ICE upgrade timed out (30s)\n");
                        break;
                    }
                }
                if (core.connection_count() >= 2) {
                    std::printf(">>> [Client] ICE connected! Data flows over UDP.\n");
                    target = "ice://" + peer_hex;
                }
            } else {
                std::fprintf(stderr, "!!! ICE upgrade: no peer pubkey available\n");
            }
        }
        if (!g_keep_running) goto shutdown;

        std::printf(">>> [Client] Connected! Firing %d worker threads.\n", threads);

        t_start = Clock::now();

        // Worker threads
        std::vector<std::thread> workers;
        workers.reserve(threads);
        for (int i = 0; i < threads; ++i) {
            workers.emplace_back([&]() {
                // Each thread owns its own payload — no false sharing.
                std::vector<uint8_t> payload(kb_size * 1024);
                randombytes_buf(payload.data(), payload.size());
                const size_t psz = payload.size();

                while (g_keep_running) {
                    if (pkts_limit > 0 &&
                        total_pkts_sent >= pkts_limit)
                        break;

                    // send() returns false if connection is not yet established or
                    // already dropped — don't count bytes in that case.
                    // This prevents phantom Tbps when the server goes down.
                    if (core.send(target.c_str(), 100, payload.data(), psz)) {
                        total_bytes_sent.fetch_add(psz, std::memory_order_relaxed);
                        total_pkts_sent .fetch_add(1,   std::memory_order_relaxed);
                    }
                }
            });
        }

        // Monitor loop
        const long interval_ms = static_cast<long>(1000.0 / report_hz);
        TimePoint last_tp   = t_start;
        uint64_t  last_bytes = 0;
        double    gbps_peak  = 0.0;

        while (g_keep_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
            if (!g_keep_running) break;

            const auto now  = Clock::now();
            const double dt = Seconds(now - last_tp).count();
            last_tp = now;

            const uint64_t cur    = total_bytes_sent;
            const uint64_t delta  = cur - last_bytes;
            last_bytes = cur;

            const double gbps   = (delta * 8.0) / dt / 1e9;
            const double total_sec = Seconds(now - t_start).count();
            const double pkt_s  = total_sec > 0
                ? total_pkts_sent / total_sec : 0;

            if (gbps > gbps_peak) gbps_peak = gbps;
            thr_samples.push(gbps);

            const double backlog_mb = core.cm().get_pending_bytes() / 1e6;

            auto st_snap = core.bus().stats_snapshot();
            if (!no_color) {
                print_live(gbps, gbps_peak, pkt_s, total_sec,
                           cur, total_pkts_sent.load(), backlog_mb, st_snap);
            } else {
                std::printf("[%.0fs] %.2f Gbps | %.0f pkt/s | Sent %s | Backlog %.1f MB\n",
                            total_sec, gbps, pkt_s,
                            fmt_bytes(static_cast<double>(cur)).c_str(), backlog_mb);
            }

            if (pkts_limit > 0 &&
                total_pkts_sent >= pkts_limit)
                break;
        }

        g_keep_running.store(false);
        for (auto& w : workers) if (w.joinable()) w.join();

        // ── Summary ──────────────────────────────────────────────────────
        const double total_sec = Seconds(Clock::now() - t_start).count();
        print_summary(thr_samples, total_sec,
                      total_bytes_sent.load(), total_pkts_sent.load(),
                      core.bus().stats_snapshot());
    } else {
        // Server-only mode — wait for signal, print live stats.
        t_start = Clock::now();
        TimePoint last_tp    = t_start;
        uint64_t  last_rx_b  = 0;
        uint64_t  last_tx_b  = 0;
        uint64_t  last_pkts  = 0;

        while (g_keep_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(
                static_cast<long>(1000.0 / report_hz)));

            const auto   now       = Clock::now();
            const double total_sec = Seconds(now - t_start).count();
            const double dt        = Seconds(now - last_tp).count();
            last_tp = now;

            const auto& st = core.bus().stats_snapshot();
            
            const uint64_t cur_rx_b  = st.rx_bytes;
            const uint64_t cur_tx_b  = st.tx_bytes;
            const uint64_t cur_pkts  = st.rx_packets;

            const double rx_gbps = dt > 0
                ? static_cast<double>(cur_rx_b - last_rx_b) * 8.0 / dt / 1e9 : 0.0;
            const double tx_gbps = dt > 0
                ? static_cast<double>(cur_tx_b - last_tx_b) * 8.0 / dt / 1e9 : 0.0;
            const double pkt_s   = dt > 0
                ? static_cast<double>(cur_pkts - last_pkts) / dt : 0.0;

            last_rx_b = cur_rx_b;
            last_tx_b = cur_tx_b;
            last_pkts = cur_pkts;
            // Only record to histogram after warmup period.
            if (total_sec > 2.0) thr_samples.push(rx_gbps);

            std::printf(
                "\033[2K\r"
                "[Server %s] "
                "conns=\033[1;32m%u\033[0m  "
                "RX \033[1;36m%.2f\033[0m Gbps  "
                "TX \033[1;33m%.2f\033[0m Gbps  "
                "%.0f pkt/s  "
                "auth✓\033[32m%llu\033[0m ✗\033[31m%llu\033[0m  "
                "dec_fail \033[%s%llu\033[0m  "
                "drops=%llu",
                fmt_duration(total_sec).c_str(),
                (unsigned)st.connections,
                rx_gbps, tx_gbps, pkt_s,
                (unsigned long long)st.auth_ok,
                (unsigned long long)st.auth_fail,
                st.decrypt_fail ? "1;31m" : "0m",
                (unsigned long long)st.decrypt_fail,
                (unsigned long long)st.backpressure);
            std::fflush(stdout);

            // --exit-after: автозавершение для CI
            if (exit_after > 0 && total_sec >= exit_after) {
                std::printf("\n>>> [Server] --exit-after %.0fs reached, stopping.\n", exit_after);
                break;
            }
        }

        const double total_sec = Seconds(Clock::now() - t_start).count();
        std::printf("\n");
        print_summary(thr_samples, total_sec,
                      total_bytes_sent.load(), total_pkts_sent.load(),
                      core.bus().stats_snapshot());
    }

    if (!target.empty()) {
        std::printf("\n>>> [Client] Sending finished. Waiting for kernel buffers to flush...\n");
        while (core.cm().get_pending_bytes() > 0 && g_keep_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

shutdown:
    std::printf("\n>>> Shutting down core...\n");
    {
        auto final_st = core.bus().stats_snapshot();
        core.stop();
        std::printf(">>> Done.\n");

        if (exit_code) {
            if (final_st.auth_fail > 0 || final_st.decrypt_fail > 0) {
                std::fprintf(stderr, "!!! Exit code 1: crypto/auth errors detected "
                             "(auth_fail=%llu, decrypt_fail=%llu)\n",
                             (unsigned long long)final_st.auth_fail,
                             (unsigned long long)final_st.decrypt_fail);
                return 1;
            }
        }
    }
    return 0;
}