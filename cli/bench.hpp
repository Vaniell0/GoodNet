#pragma once
/// @file cli/bench.hpp
/// @brief Benchmark engine: multi-threaded sender with live monitoring.

#include <atomic>
#include <cstdint>
#include <string>

#include "dashboard.hpp"
#include "core.hpp"

namespace cli {

struct BenchConfig {
    std::string target;
    int         threads      = 4;
    uint64_t    pkt_limit    = 0;       ///< 0 = unlimited
    size_t      pkt_size_kb  = 64;
    double      report_hz    = 2.0;
    double      timeout      = 30.0;    ///< Handshake timeout in seconds
    bool        no_color     = false;
    bool        ice_upgrade  = false;
};

struct BenchResult {
    double    total_sec    = 0;
    uint64_t  total_bytes  = 0;
    uint64_t  total_pkts   = 0;
    Samples   throughput;
    gn::StatsSnapshot stats;
    int       exit_status  = 0;         ///< 0=ok, 1=timeout, 2=crypto error
};

/// Run the benchmark: connect, optional ICE upgrade, fire workers, monitor.
/// Blocks until done or keep_running becomes false.
BenchResult run_benchmark(gn::Core& core, const BenchConfig& cfg,
                          std::atomic<bool>& keep_running);

} // namespace cli
