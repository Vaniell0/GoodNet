/// @file plugins/logger/logger.cpp
/// @brief Built-in handler that LOG_INFO-logs every packet passing through the core.
///
/// What it logs (one line per packet, level INFO):
///   [MsgLogger] <direction> type=<hex> id=<conn_id> from=<addr>:<port>
///               size=<payload_bytes> ts=<unix_us> pk=<peer_pubkey_prefix>
///
/// Subscribes as a wildcard listener — receives ALL message types without
/// touching the payload bytes themselves (no copy, header pointer only).

#include <handler.hpp>
#include <plugin.hpp>
#include <logger.hpp>
#include <fmt/format.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>

namespace gn {

class MsgLogger final : public IHandler {
public:
    // ── IHandler interface ────────────────────────────────────────────────────

    const char* get_plugin_name() const override { return "MsgLogger"; }

    void on_init() override {
        // Wildcard: receive every message type.
        // MSG_TYPE_AUTH (1) is included so key-exchange can be audited.
        set_supported_types({0, 1, 2, 100, 200});
        start_ts_ = now_us();
        LOG_INFO("[MsgLogger] started — logging all packet types");
    }

    void handle_message(const header_t*   hdr,
                        const endpoint_t* ep,
                        const void*       /*payload*/,
                        size_t            payload_size) override
    {
        if (!hdr) return;

        const uint64_t count = ++count_;

        // Peer address string
        const char* addr = (ep && ep->address[0]) ? ep->address : "?";
        const uint16_t port = ep ? ep->port : 0;

        // Peer pubkey prefix (first 4 bytes → 8 hex chars) — enough to identify
        char pk_prefix[9] = "????????";
        if (ep) {
            std::snprintf(pk_prefix, sizeof(pk_prefix),
                          "%02x%02x%02x%02x",
                          ep->pubkey[0], ep->pubkey[1],
                          ep->pubkey[2], ep->pubkey[3]);
        }

        // Human-readable type name for common types
        const char* type_name = type_str(hdr->payload_type);

        LOG_INFO("[MsgLogger] #{} type={} ({}) from={}:{} size={} ts={} pk={}...",
                 count,
                 hdr->payload_type, type_name,
                 addr, port,
                 payload_size,
                 hdr->timestamp,
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
            case 0:   return "SYSTEM";
            case 1:   return "AUTH";
            case 2:   return "KEY_EXCHANGE";
            case 3:   return "HEARTBEAT";
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
