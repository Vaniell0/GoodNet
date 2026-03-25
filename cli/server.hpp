#pragma once
/// @file cli/server.hpp
/// @brief Server-mode monitor: live stats dashboard until signal or --exit-after.

#include <atomic>
#include <cstdint>

#include "dashboard.hpp"
#include "core.hpp"

namespace cli {

struct ServerConfig {
    double report_hz   = 2.0;
    double exit_after  = 0.0;   ///< 0 = run forever until signal
    bool   no_color    = false;
};

struct ServerResult {
    double    total_sec   = 0;
    Samples   throughput;
    gn::StatsSnapshot stats;
};

/// Run the server monitor loop. Blocks until keep_running becomes false
/// or --exit-after time elapses.
ServerResult run_server(gn::Core& core, const ServerConfig& cfg,
                        std::atomic<bool>& keep_running);

} // namespace cli
