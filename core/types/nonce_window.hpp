#pragma once

#include <bitset>
#include <cstdint>
#include <mutex>

namespace gn {

/// Sliding-window anti-replay filter (IPsec/DTLS-style).
/// Accepts nonces within WINDOW_SIZE of the highest seen nonce;
/// rejects duplicates and nonces older than the window.
struct NonceWindow {
    static constexpr size_t WINDOW_SIZE = 256;

    std::mutex mu;
    uint64_t   highest_nonce{0};
    std::bitset<WINDOW_SIZE> bitmap{};

    /// Returns true if @p nonce is valid (not a replay, within window).
    bool accept(uint64_t nonce);

    /// Reset state (used after rekey).
    void reset();
};

} // namespace gn
