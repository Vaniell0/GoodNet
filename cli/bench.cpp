/// @file cli/bench.cpp
/// @brief Benchmark engine implementation.

#include "bench.hpp"

#include <chrono>
#include <thread>
#include <vector>

#include <sodium.h>

#include "cm/connectionManager.hpp"
#include "pm/pluginManager.hpp"
#include "util.hpp"

using Clock     = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Seconds   = std::chrono::duration<double>;

namespace cli {

// ─── ICE upgrade helper ─────────────────────────────────────────────────────

static conn_id_t try_ice_upgrade(gn::Core& core, conn_id_t primary,
                                 std::atomic<bool>& keep_running) {
    std::string peer_hex;
    for (auto cid : core.active_conn_ids()) {
        auto pk = core.peer_pubkey(cid);
        if (!pk.empty()) {
            peer_hex = gn::bytes_to_hex(pk.data(), pk.size());
            break;
        }
    }
    if (peer_hex.empty()) {
        std::fprintf(stderr, "!!! ICE upgrade: no peer pubkey available\n");
        return primary;
    }

    std::printf(">>> [Client] ICE upgrade: negotiating...\n");
    core.connect("ice://" + peer_hex);

    auto ice_start = Clock::now();
    while (keep_running.load(std::memory_order_relaxed) &&
           core.connection_count() < 2) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (Seconds(Clock::now() - ice_start).count() > 30.0) {
            std::fprintf(stderr, "!!! ICE upgrade timed out (30s)\n");
            return primary;
        }
    }

    if (core.connection_count() >= 2) {
        std::printf(">>> [Client] ICE connected! Data flows over UDP.\n");
        for (auto cid : core.active_conn_ids()) {
            if (cid != primary) return cid;
        }
    }
    return primary;
}

// ─── Benchmark ──────────────────────────────────────────────────────────────

BenchResult run_benchmark(gn::Core& core, const BenchConfig& cfg,
                          std::atomic<bool>& keep_running) {
    BenchResult result;

    // ── Connect + handshake wait ─────────────────────────────────────────────
    std::printf(">>> [Client] Connecting to %s ...\n", cfg.target.c_str());
    core.connect(cfg.target);

    auto t_wait = Clock::now();
    while (keep_running.load(std::memory_order_relaxed) &&
           core.active_conn_ids().empty()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (Seconds(Clock::now() - t_wait).count() > cfg.timeout) {
            std::fprintf(stderr,
                "!!! Handshake timeout (%.0fs). Is the server running?\n",
                cfg.timeout);
            result.exit_status = 1;
            return result;
        }
    }

    if (!keep_running.load(std::memory_order_relaxed)) return result;

    conn_id_t primary_conn = core.active_conn_ids().front();

    // ── ICE upgrade (optional) ───────────────────────────────────────────────
    if (cfg.ice_upgrade) {
        primary_conn = try_ice_upgrade(core, primary_conn, keep_running);
    }

    if (!keep_running.load(std::memory_order_relaxed)) return result;

    std::printf(">>> [Client] Connected! Firing %d worker threads.\n", cfg.threads);

    // ── Worker threads ───────────────────────────────────────────────────────
    std::atomic<uint64_t> total_bytes_sent{0};
    std::atomic<uint64_t> total_pkts_sent{0};

    const auto t_start = Clock::now();

    std::vector<std::thread> workers;
    workers.reserve(cfg.threads);
    for (int i = 0; i < cfg.threads; ++i) {
        workers.emplace_back([&, primary_conn]() {
            std::vector<uint8_t> payload(cfg.pkt_size_kb * 1024);
            randombytes_buf(payload.data(), payload.size());
            const size_t psz = payload.size();

            while (keep_running.load(std::memory_order_relaxed)) {
                if (cfg.pkt_limit > 0 &&
                    total_pkts_sent.load(std::memory_order_relaxed) >= cfg.pkt_limit)
                    break;

                if (core.send(primary_conn, 100,
                              std::span{payload.data(), psz})) {
                    total_bytes_sent.fetch_add(psz, std::memory_order_relaxed);
                    total_pkts_sent.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    // ── Monitor loop ─────────────────────────────────────────────────────────
    const long interval_ms = static_cast<long>(1000.0 / cfg.report_hz);
    TimePoint last_tp   = t_start;
    uint64_t  last_bytes = 0;
    double    gbps_peak  = 0.0;

    while (keep_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
        if (!keep_running.load(std::memory_order_relaxed)) break;

        const auto now   = Clock::now();
        const double dt  = Seconds(now - last_tp).count();
        last_tp = now;

        const uint64_t cur   = total_bytes_sent.load(std::memory_order_relaxed);
        const uint64_t delta = cur - last_bytes;
        last_bytes = cur;

        const double gbps      = (delta * 8.0) / dt / 1e9;
        const double total_sec = Seconds(now - t_start).count();
        const double pkt_s     = total_sec > 0
            ? total_pkts_sent.load(std::memory_order_relaxed) / total_sec : 0;

        if (gbps > gbps_peak) gbps_peak = gbps;
        result.throughput.push(gbps);

        const double backlog_mb = core.cm().get_pending_bytes() / 1e6;
        auto st_snap = core.bus().stats_snapshot();

        if (!cfg.no_color) {
            print_live(gbps, gbps_peak, pkt_s, total_sec,
                       cur, total_pkts_sent.load(std::memory_order_relaxed),
                       backlog_mb, st_snap);
        } else {
            std::printf("[%.0fs] %.2f Gbps | %.0f pkt/s | Sent %s | Backlog %.1f MB\n",
                        total_sec, gbps, pkt_s,
                        fmt_bytes(static_cast<double>(cur)).c_str(), backlog_mb);
        }

        if (cfg.pkt_limit > 0 &&
            total_pkts_sent.load(std::memory_order_relaxed) >= cfg.pkt_limit)
            break;
    }

    keep_running.store(false, std::memory_order_relaxed);
    for (auto& w : workers) if (w.joinable()) w.join();

    // ── Flush pending writes ─────────────────────────────────────────────────
    std::printf("\n>>> [Client] Sending finished. Waiting for kernel buffers to flush...\n");
    auto flush_start = Clock::now();
    while (core.cm().get_pending_bytes() > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (Seconds(Clock::now() - flush_start).count() > 5.0) break;
    }

    // ── Fill result ──────────────────────────────────────────────────────────
    result.total_sec   = Seconds(Clock::now() - t_start).count();
    result.total_bytes = total_bytes_sent.load(std::memory_order_relaxed);
    result.total_pkts  = total_pkts_sent.load(std::memory_order_relaxed);
    result.stats       = core.bus().stats_snapshot();

    return result;
}

} // namespace cli
