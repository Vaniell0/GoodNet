/// @file cli/server.cpp
/// @brief Server-mode monitor implementation.

#include "server.hpp"

#include <chrono>
#include <cstdio>
#include <thread>

using Clock     = std::chrono::steady_clock;
using Seconds   = std::chrono::duration<double>;

namespace cli {

ServerResult run_server(gn::Core& core, const ServerConfig& cfg,
                        std::atomic<bool>& keep_running) {
    ServerResult result;

    const auto t_start = Clock::now();
    auto last_tp  = t_start;
    uint64_t last_rx_b = 0, last_tx_b = 0, last_pkts = 0;

    while (keep_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(
            static_cast<long>(1000.0 / cfg.report_hz)));

        const auto   now       = Clock::now();
        const double total_sec = Seconds(now - t_start).count();
        const double dt        = Seconds(now - last_tp).count();
        last_tp = now;

        const auto st = core.bus().stats_snapshot();

        const uint64_t cur_rx_b = st.rx_bytes;
        const uint64_t cur_tx_b = st.tx_bytes;
        const uint64_t cur_pkts = st.rx_packets;

        const double rx_gbps = dt > 0
            ? static_cast<double>(cur_rx_b - last_rx_b) * 8.0 / dt / 1e9 : 0.0;
        const double tx_gbps = dt > 0
            ? static_cast<double>(cur_tx_b - last_tx_b) * 8.0 / dt / 1e9 : 0.0;
        const double pkt_s   = dt > 0
            ? static_cast<double>(cur_pkts - last_pkts) / dt : 0.0;

        last_rx_b = cur_rx_b;
        last_tx_b = cur_tx_b;
        last_pkts = cur_pkts;

        // Записываем в histogram после прогрева (2 секунды).
        if (total_sec > 2.0) result.throughput.push(rx_gbps);

        if (!cfg.no_color) {
            print_server_line(total_sec, st.connections,
                              rx_gbps, tx_gbps, pkt_s, st);
        } else {
            std::printf("[Server %.0fs] conns=%u  RX %.2f Gbps  TX %.2f Gbps  "
                        "%.0f pkt/s  auth ok=%llu fail=%llu\n",
                        total_sec, (unsigned)st.connections,
                        rx_gbps, tx_gbps, pkt_s,
                        (unsigned long long)st.auth_ok,
                        (unsigned long long)st.auth_fail);
        }

        if (cfg.exit_after > 0 && total_sec >= cfg.exit_after) {
            std::printf("\n>>> [Server] --exit-after %.0fs reached, stopping.\n",
                        cfg.exit_after);
            break;
        }
    }

    result.total_sec = Seconds(Clock::now() - t_start).count();
    result.stats     = core.bus().stats_snapshot();
    std::printf("\n");

    return result;
}

} // namespace cli
