/// @file src/capi.cpp

#include "core.h"
#include "core.hpp"
#include "config.hpp"
#include "signals.hpp"
#include "version.hpp"

#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>

static_assert(static_cast<size_t>(gn::DropReason::_Count) == GN_DROP_REASON_COUNT,
              "GN_DROP_REASON_COUNT must match DropReason::_Count");

// Wrapper: owns both Config and Core so CAPI lifetime is clean.
struct CoreHandle {
    Config                    config;
    std::unique_ptr<gn::Core> core;

    explicit CoreHandle() : config(true) {}
};

static inline gn::Core* to_core(gn_core_t* h) {
    auto* ch = reinterpret_cast<CoreHandle*>(h);
    return (ch && ch->core) ? ch->core.get() : nullptr;
}

extern "C" {

// ── Lifecycle ─────────────────────────────────────────────────────────────────

gn_core_t* gn_core_create(gn_config_t* cfg) {
    auto* h = new CoreHandle();
    if (cfg) {
        if (cfg->config_dir) h->config.identity.dir  = cfg->config_dir;
        if (cfg->log_level)  h->config.logging.level  = cfg->log_level;
        h->config.core.listen_port = static_cast<int>(cfg->listen_port);
    }
    h->core = std::make_unique<gn::Core>(&h->config);
    return reinterpret_cast<gn_core_t*>(h);
}

void gn_core_destroy(gn_core_t* core) {
    delete reinterpret_cast<CoreHandle*>(core);
}

void gn_core_run(gn_core_t* core) {
    if (auto* c = to_core(core)) c->run();
}

void gn_core_run_async(gn_core_t* core, int threads) {
    if (auto* c = to_core(core)) c->run_async(threads);
}

void gn_core_stop(gn_core_t* core) {
    if (auto* c = to_core(core)) c->stop();
}

int gn_core_reload_config(gn_core_t* core) {
    auto* c = to_core(core);
    if (!c) return -1;
    return c->reload_config() ? 0 : -1;
}

int gn_core_is_running(gn_core_t* core) {
    auto* c = to_core(core);
    return (c && c->is_running()) ? 1 : 0;
}

// ── Network ───────────────────────────────────────────────────────────────────

void gn_core_send(gn_core_t* core, const char* uri, uint32_t type,
                   const void* data, size_t len) {
    if (auto* c = to_core(core))
        c->send(std::string_view(uri), type,
                std::span{static_cast<const uint8_t*>(data), len});
}

void gn_core_broadcast(gn_core_t* core, uint32_t type,
                        const void* data, size_t len) {
    if (auto* c = to_core(core))
        c->broadcast(type, std::span{static_cast<const uint8_t*>(data), len});
}

void gn_core_disconnect(gn_core_t* core, uint64_t conn_id) {
    if (auto* c = to_core(core)) c->disconnect(conn_id);
}

int gn_core_rekey(gn_core_t* core, uint64_t conn_id) {
    auto* c = to_core(core);
    if (!c) return -1;
    return c->rekey_session(conn_id) ? 0 : -1;
}

// ── Identity ──────────────────────────────────────────────────────────────────

size_t gn_core_get_user_pubkey(gn_core_t* core, char* buffer, size_t max_len) {
    auto* c = to_core(core);
    if (!c || !buffer || max_len == 0) return 0;
    const auto hex = c->user_pubkey_hex();
    const size_t len = std::min(hex.size(), max_len - 1);
    std::memcpy(buffer, hex.c_str(), len);
    buffer[len] = '\0';
    return len;
}

// ── Stats ─────────────────────────────────────────────────────────────────────

void gn_core_get_stats(gn_core_t* core, gn_stats_t* out) {
    auto* c = to_core(core);
    if (!c || !out) return;
    const gn::StatsSnapshot s = c->stats_snapshot();
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

uint32_t gn_core_connection_count(gn_core_t* core) {
    auto* c = to_core(core);
    return c ? static_cast<uint32_t>(c->connection_count()) : 0;
}

// ── Diagnostics ──────────────────────────────────────────────────────────────

size_t gn_core_dump_connections(gn_core_t* core, char* buf, size_t max_len) {
    auto* c = to_core(core);
    if (!c || !buf || max_len == 0) return 0;
    const auto json = c->dump_connections();
    const size_t len = std::min(json.size(), max_len - 1);
    std::memcpy(buf, json.c_str(), len);
    buf[len] = '\0';
    return len;
}

uint32_t gn_core_handler_count(gn_core_t* core) {
    auto* c = to_core(core);
    return c ? static_cast<uint32_t>(c->handler_count()) : 0;
}

uint32_t gn_core_connector_count(gn_core_t* core) {
    auto* c = to_core(core);
    return c ? static_cast<uint32_t>(c->connector_count()) : 0;
}

const char* gn_version(void) {
    return GOODNET_VERSION_STRING;
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
    auto* c = to_core(core);
    if (!c || !cb) return 0;

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

    c->subscribe(type, std::to_string(token), std::move(handler));

    return token;
}

void gn_core_unsubscribe(gn_core_t* /*core*/, uint64_t sub_id) {
    std::lock_guard lock(g_csub_mu);
    g_csub_map.erase(sub_id);
}

}  // extern "C"
