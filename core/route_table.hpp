#pragma once
/// @file core/route_table.hpp
/// @brief Route table for learned-route forwarding (replaces pure gossip relay).

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../sdk/types.h"

namespace gn {

std::string bytes_to_hex(const uint8_t* data, size_t len);

struct RouteEntry {
    std::array<uint8_t, 32> dest_pubkey;
    conn_id_t next_hop;           ///< Direct connection to forward through
    uint8_t   hop_count;
    uint32_t  seq_num;            ///< For conflict resolution
    std::chrono::steady_clock::time_point expires;
};

/// @brief Thread-safe route table with pessimistic decay GC.
///
/// Routes are learned from ROUTE_ANNOUNCE messages. find_route() returns
/// the best next-hop for a given destination pubkey. Entries that are not
/// refreshed for DECAY_THRESHOLD get hop_count incremented each GC cycle;
/// once hop_count exceeds MAX_HOPS the route is removed.
class RouteTable {
public:
    static constexpr uint8_t MAX_HOPS = 16;
    static constexpr auto    DECAY_THRESHOLD = std::chrono::minutes(5);

    /// Find best route to destination pubkey.
    std::optional<conn_id_t> find_route(const uint8_t dest_pubkey[32]) const {
        const std::string hex = bytes_to_hex(dest_pubkey, 32);
        std::shared_lock lk(mu_);
        auto it = routes_.find(hex);
        if (it == routes_.end()) return std::nullopt;
        return it->second.next_hop;
    }

    /// Update/insert route entry (from ROUTE_ANNOUNCE).
    void update(const RouteEntry& entry) {
        const std::string hex = bytes_to_hex(entry.dest_pubkey.data(), 32);
        std::unique_lock lk(mu_);
        auto it = routes_.find(hex);
        if (it != routes_.end()) {
            // Prefer higher seq_num, or lower hop_count at same seq
            if (entry.seq_num < it->second.seq_num) return;
            if (entry.seq_num == it->second.seq_num &&
                entry.hop_count >= it->second.hop_count) return;
        }
        routes_[hex] = entry;
    }

    /// Remove all routes via a specific connection (on disconnect).
    void remove_via(conn_id_t conn) {
        std::unique_lock lk(mu_);
        for (auto it = routes_.begin(); it != routes_.end(); ) {
            if (it->second.next_hop == conn)
                it = routes_.erase(it);
            else
                ++it;
        }
    }

    /// Pessimistic decay GC: stale entries get hop_count incremented;
    /// removed when hop_count exceeds MAX_HOPS.
    void gc() {
        const auto now = std::chrono::steady_clock::now();
        std::unique_lock lk(mu_);
        for (auto it = routes_.begin(); it != routes_.end(); ) {
            if (now > it->second.expires) {
                it = routes_.erase(it);
            } else if (now - it->second.expires + DECAY_THRESHOLD >
                       DECAY_THRESHOLD) {
                // Entry not refreshed recently — decay
                if (++it->second.hop_count > MAX_HOPS)
                    it = routes_.erase(it);
                else
                    ++it;
            } else {
                ++it;
            }
        }
    }

    /// Get all routes (for periodic announce).
    std::vector<RouteEntry> snapshot() const {
        std::shared_lock lk(mu_);
        std::vector<RouteEntry> result;
        result.reserve(routes_.size());
        for (auto& [_, entry] : routes_)
            result.push_back(entry);
        return result;
    }

    /// Number of known routes.
    size_t size() const {
        std::shared_lock lk(mu_);
        return routes_.size();
    }

private:
    mutable std::shared_mutex mu_;
    std::unordered_map<std::string, RouteEntry> routes_;
};

} // namespace gn
