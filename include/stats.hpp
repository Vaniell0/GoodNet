#pragma once
#include <atomic>
#include <vector>
#include <string>
#include <chrono>

namespace gn {

struct Stats {
    static inline size_t total_handlers = 0;
    static inline size_t enabled_handlers = 0;
    static inline size_t total_connectors = 0;
    static inline size_t enabled_connectors = 0;
    
    static inline std::vector<int64_t> load_times;
    static inline std::vector<std::string> loaded_handlers;
    static inline std::vector<std::string> loaded_connectors;

    static inline std::atomic<size_t> plugin_count = 0;
    static inline std::atomic<size_t> packets_sent = 0;
    static inline std::atomic<size_t> connection_count = 0;
    static inline std::atomic<size_t> packets_received = 0;
    static inline std::chrono::system_clock::time_point start_time = std::chrono::system_clock::now();
    static inline std::atomic<bool> is_running = false;
    static inline std::atomic<bool> is_initialized = false;
};

} // namespace gn
