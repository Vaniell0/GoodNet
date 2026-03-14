/// @file src/capi.cpp

#include "core.h"
#include "core.hpp"
#include "signals.hpp"

#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>

extern "C" {

// ── Lifecycle ─────────────────────────────────────────────────────────────────

gn_core_t* gn_core_create(gn_config_t* cfg) {
    gn::CoreConfig cpp_cfg;
    if (cfg) {
        if (cfg->config_dir) cpp_cfg.identity.dir  = cfg->config_dir;
        if (cfg->log_level)  cpp_cfg.logging.level = cfg->log_level;
        cpp_cfg.network.listen_port                 = cfg->listen_port;
    }
    return reinterpret_cast<gn_core_t*>(new gn::Core(std::move(cpp_cfg)));
}

void gn_core_destroy(gn_core_t* core) {
    delete reinterpret_cast<gn::Core*>(core);
}

void gn_core_run(gn_core_t* core) {
    if (core) reinterpret_cast<gn::Core*>(core)->run();
}

void gn_core_run_async(gn_core_t* core, int threads) {
    if (core) reinterpret_cast<gn::Core*>(core)->run_async(threads);
}

void gn_core_stop(gn_core_t* core) {
    if (core) reinterpret_cast<gn::Core*>(core)->stop();
}

int gn_core_reload_config(gn_core_t* core) {
    if (!core) return -1;
    return reinterpret_cast<gn::Core*>(core)->reload_config() ? 0 : -1;
}

// ── Network ───────────────────────────────────────────────────────────────────

void gn_core_send(gn_core_t* core, const char* uri, uint32_t type,
                   const void* data, size_t len) {
    if (core) reinterpret_cast<gn::Core*>(core)->send(uri, type, data, len);
}

void gn_core_broadcast(gn_core_t* core, uint32_t type,
                        const void* data, size_t len) {
    if (core) reinterpret_cast<gn::Core*>(core)->broadcast(type, data, len);
}

void gn_core_disconnect(gn_core_t* core, uint64_t conn_id) {
    if (core) reinterpret_cast<gn::Core*>(core)->disconnect(conn_id);
}

int gn_core_rekey(gn_core_t* core, uint64_t conn_id) {
    if (!core) return -1;
    return reinterpret_cast<gn::Core*>(core)->rekey_session(conn_id) ? 0 : -1;
}

// ── Identity ──────────────────────────────────────────────────────────────────

size_t gn_core_get_user_pubkey(gn_core_t* core, char* buffer, size_t max_len) {
    if (!core || !buffer || max_len == 0) return 0;
    const auto hex = reinterpret_cast<gn::Core*>(core)->user_pubkey_hex();
    const size_t len = std::min(hex.size(), max_len - 1);
    std::memcpy(buffer, hex.c_str(), len);
    buffer[len] = '\0';
    return len;
}

// ── Stats ─────────────────────────────────────────────────────────────────────

void gn_core_get_stats(gn_core_t* core, gn_stats_t* out) {
    if (!core || !out) return;
    const gn::StatsSnapshot s =
        reinterpret_cast<gn::Core*>(core)->stats_snapshot();
    out->rx_bytes        = s.rx_bytes;
    out->tx_bytes        = s.tx_bytes;
    out->rx_packets      = s.rx_packets;
    out->tx_packets      = s.tx_packets;
    out->auth_ok         = s.auth_ok;
    out->auth_fail       = s.auth_fail;
    out->decrypt_fail    = s.decrypt_fail;
    out->backpressure    = s.backpressure;
    out->consumed        = s.consumed;
    out->rejected        = s.rejected;
    out->connections     = s.connections;
    out->total_conn      = s.total_conn;
    out->total_disc      = s.total_disc;
    static_assert(sizeof(out->drops) == sizeof(s.drops),
                  "gn_stats_t::drops size mismatch");
    std::memcpy(out->drops, s.drops, sizeof(s.drops));
    out->dispatch_lat_avg = s.dispatch_latency.avg_ns();
}

// ── Subscriptions ─────────────────────────────────────────────────────────────

namespace {

struct CSubState {
    gn_handler_fn cb        = nullptr;
    void*         user_data = nullptr;
};

std::mutex                               g_csub_mu;
std::unordered_map<uint64_t, CSubState>  g_csub_map;
std::atomic<uint64_t>                    g_csub_token{1};

}  // namespace

uint64_t gn_core_subscribe(gn_core_t* core, uint32_t type,
                             gn_handler_fn cb, void* user_data) {
    if (!core || !cb) return 0;

    const uint64_t token = g_csub_token.fetch_add(1, std::memory_order_relaxed);

    {
        std::lock_guard lock(g_csub_mu);
        g_csub_map.emplace(token, CSubState{cb, user_data});
    }

    auto handler = [token](std::string_view,
                            std::shared_ptr<header_t> hdr,
                            const endpoint_t*,
                            gn::PacketData data) -> propagation_t {
        std::lock_guard lock(g_csub_mu);
        auto it = g_csub_map.find(token);
        if (it == g_csub_map.end()) return PROPAGATION_CONTINUE;
        return it->second.cb(hdr->payload_type,
                              data->data(), data->size(),
                              it->second.user_data);
    };

    reinterpret_cast<gn::Core*>(core)->subscribe(
        type, std::to_string(token), std::move(handler));

    return token;
}

void gn_core_unsubscribe(gn_core_t* /*core*/, uint64_t sub_id) {
    std::lock_guard lock(g_csub_mu);
    g_csub_map.erase(sub_id);
}

}  // extern "C"
