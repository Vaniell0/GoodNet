/// @file plugins/logger/logger.cpp
/// @brief Built-in handler that LOG_INFO-logs every packet passing through the core.
///
/// What it logs (one line per packet, level INFO):
///   [MsgLogger] <direction> type=<hex> id=<conn_id> from=<addr>:<port>
///               size=<payload_bytes> ts=<unix_us> pk=<peer_pubkey_prefix>
///
/// Subscribes as a wildcard listener — receives ALL message types without
/// touching the payload bytes themselves (no copy, header pointer only).

#include <fmt/format.h>
#include <handler.hpp>
#include <logger.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>

namespace gn {

class MsgLogger final : public IHandler {
public:
    // ── IHandler interface ────────────────────────────────────────────────────

    const char* get_plugin_name() const override { return "MsgLogger"; }

    void on_init() override {
        // Wildcard: receive every message type.
        set_supported_types({1, 2, 3, 4, 10, 11, 100, 200});
        start_ts_ = now_us();
        LOG_INFO("[MsgLogger] started — logging all packet types");
    }

    void handle_message(const header_t* header,
                        const endpoint_t* endpoint,
                        std::span<const uint8_t> payload) override
    {
        if (!header) return;

        const uint64_t count = ++count_;

        // Peer address string
        const char* addr = (endpoint && endpoint->address[0]) ? endpoint->address : "?";
        const uint16_t port = endpoint ? endpoint->port : 0;

        // Peer pubkey prefix (first 4 bytes → 8 hex chars) — enough to identify
        char pk_prefix[9] = "????????";
        if (endpoint) {
            std::snprintf(pk_prefix, sizeof(pk_prefix),
                          "%02x%02x%02x%02x",
                          endpoint->pubkey[0], endpoint->pubkey[1],
                          endpoint->pubkey[2], endpoint->pubkey[3]);
        }

        // Human-readable type name for common types
        const char* type_name = type_str(header->payload_type);

        LOG_INFO("[MsgLogger] #{} type={} ({}) from={}:{} size={} pkt={} pk={}...",
                 count,
                 header->payload_type, type_name,
                 addr, port,
                 payload.size(),
                 header->packet_id,
                 pk_prefix);
    }

    void on_shutdown() override {
        const uint64_t elapsed_ms = (now_us() - start_ts_) / 1000;
        LOG_INFO("[MsgLogger] stopped — {} packets logged in {} ms",
                 count_.load(), elapsed_ms);
    }

    // ── Factory for embedded use (no .so) ────────────────────────────────────

    /// @brief Create an instance without going through the plugin system.
    ///        Call node.register_handler(MsgLogger::create()) to embed.
    static std::unique_ptr<MsgLogger> create() {
        return std::make_unique<MsgLogger>();
    }

private:
    static const char* type_str(uint32_t t) noexcept {
        switch (t) {
            case 1:   return "NOISE_INIT";
            case 2:   return "NOISE_RESP";
            case 3:   return "NOISE_FIN";
            case 4:   return "HEARTBEAT";
            case 10:  return "RELAY";
            case 11:  return "ICE_SIGNAL";
            case 100: return "CHAT";
            case 200: return "FILE";
            default:  return "USER";
        }
    }

    static uint64_t now_us() {
        using namespace std::chrono;
        return (uint64_t)duration_cast<microseconds>(
            steady_clock::now().time_since_epoch()).count();
    }

    std::atomic<uint64_t> count_{0};
    uint64_t              start_ts_{0};
};

} // namespace gn

// ── Plugin export (for .so build) ─────────────────────────────────────────────
HANDLER_PLUGIN(gn::MsgLogger)