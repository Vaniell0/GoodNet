/// @file core/cm_send.cpp

#include "connectionManager.hpp"
#include "logger.hpp"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <sys/uio.h>

#include "../sdk/connector.h"

namespace gn {

std::string bytes_to_hex(const uint8_t* data, size_t len);

// ── Helpers ───────────────────────────────────────────────────────────────────

bool ConnectionManager::is_localhost_address(std::string_view a) {
    return a == "127.0.0.1" || a == "::1" || a == "localhost" || a.starts_with("127.");
}

connector_ops_t* ConnectionManager::find_connector(const std::string& scheme) {
    std::shared_lock lk(connectors_mu_);
    auto it = connectors_.find(scheme.empty() ? "tcp" : scheme);
    return it != connectors_.end() ? it->second : nullptr;
}

std::optional<conn_id_t> ConnectionManager::resolve_uri(std::string_view uri) const {
    std::string key(uri);
    if (auto sep = key.find("://"); sep != std::string::npos)
        key = key.substr(sep + 3);
    std::shared_lock lk(uri_mu_);
    auto it = uri_index_.find(key);
    return it != uri_index_.end() ? std::optional{it->second} : std::nullopt;
}

std::vector<std::string> ConnectionManager::local_schemes() const {
    std::shared_lock lk(connectors_mu_);
    std::vector<std::string> out; out.reserve(connectors_.size());
    for (auto& [s, _] : connectors_) out.push_back(s);
    return out;
}

std::string ConnectionManager::negotiate_scheme(const ConnectionRecord& rec) const {
    const auto local = local_schemes();
    for (auto& prio : scheme_priority_) {
        if (std::find(local.begin(), local.end(), prio) == local.end()) continue;
        if (rec.peer_schemes.empty()) return prio;
        if (std::find(rec.peer_schemes.begin(), rec.peer_schemes.end(), prio)
                != rec.peer_schemes.end())
            return prio;
    }
    return local.empty() ? "tcp" : local.front();
}

std::shared_ptr<PerConnQueue> ConnectionManager::get_or_create_queue(conn_id_t id) {
    {
        std::shared_lock lk(queues_mu_);
        if (auto it = send_queues_.find(id); it != send_queues_.end())
            return it->second;
    }
    std::unique_lock lk(queues_mu_);
    auto& q = send_queues_[id];
    if (!q) q = std::make_shared<PerConnQueue>();
    return q;
}

uint64_t ConnectionManager::monotonic_ns() noexcept {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL
         + static_cast<uint64_t>(ts.tv_nsec);
}

// ── build_frame ───────────────────────────────────────────────────────────────

std::vector<uint8_t> ConnectionManager::build_frame(conn_id_t id,
                                                      uint32_t msg_type,
                                                      std::span<const uint8_t> payload) {
    auto rec = rcu_find(id);
    if (!rec) return {};

    thread_local std::vector<uint8_t> tl_enc;

    const bool do_encrypt = (msg_type != MSG_TYPE_AUTH)
                         && (msg_type != MSG_TYPE_KEY_EXCHANGE)
                         && !rec->is_localhost
                         && rec->session
                         && rec->session->ready;

    std::span<const uint8_t> final_payload = payload;

    if (do_encrypt) {
        tl_enc = rec->session->encrypt(payload.data(), payload.size());
        final_payload = tl_enc;
    }

    header_t hdr{};
    hdr.magic        = GNET_MAGIC;
    hdr.proto_ver    = GNET_PROTO_VER;
    hdr.payload_type = static_cast<uint16_t>(msg_type);
    hdr.payload_len  = static_cast<uint32_t>(final_payload.size());
    hdr.packet_id    = rec->send_packet_id.fetch_add(1, std::memory_order_relaxed);

    struct timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    hdr.timestamp = static_cast<uint64_t>(ts.tv_sec) * 1'000'000ULL
                  + static_cast<uint64_t>(ts.tv_nsec) / 1'000ULL;

    {
        std::shared_lock lk(identity_mu_);
        std::memcpy(hdr.sender_id, identity_.device_pubkey, sizeof(hdr.sender_id));
    }

    std::vector<uint8_t> frame(sizeof(header_t) + final_payload.size());
    std::memcpy(frame.data(), &hdr, sizeof(header_t));
    if (!final_payload.empty())
        std::memcpy(frame.data() + sizeof(header_t),
                    final_payload.data(), final_payload.size());
    return frame;
}

// ── flush_frames_to_connector ─────────────────────────────────────────────────

void ConnectionManager::flush_frames_to_connector(
        conn_id_t id, connector_ops_t* ops,
        std::vector<std::vector<uint8_t>>& frames) {
    if (frames.empty()) return;

    if (ops->send_gather && frames.size() > 1) {
        // Gather IO: one syscall for the whole batch
        std::vector<struct iovec> iov;
        iov.reserve(frames.size());
        for (auto& f : frames)
            iov.push_back({f.data(), f.size()});

        const int rc = ops->send_gather(ops->connector_ctx, id,
                                         iov.data(), static_cast<int>(iov.size()));
        if (rc < 0) {
            LOG_ERROR("send_gather #{}: connector error", id);
            return;
        }
        size_t total = 0;
        for (auto& f : frames) total += f.size();
        bus_.emit_stat({StatsEvent::Kind::TxBytes,  total, id});
        bus_.emit_stat({StatsEvent::Kind::TxPacket, (uint64_t)frames.size(), id});
    } else {
        // Fall back to individual send_to()
        for (auto& f : frames) {
            const int rc = ops->send_to(ops->connector_ctx, id,
                                         f.data(), f.size());
            if (rc == 0) {
                bus_.emit_stat({StatsEvent::Kind::TxBytes,  f.size(), id});
                bus_.emit_stat({StatsEvent::Kind::TxPacket, 1,        id});
            } else {
                LOG_ERROR("send_to #{}: connector error", id);
            }
        }
    }
}

// ── send_frame ────────────────────────────────────────────────────────────────

void ConnectionManager::send_frame(conn_id_t id, uint32_t msg_type,
                                    std::span<const uint8_t> payload) {
    auto frame = build_frame(id, msg_type, payload);
    if (frame.empty()) return;

    auto rec = rcu_find(id);
    if (!rec) return;
    const std::string scheme = rec->negotiated_scheme.empty()
                             ? rec->local_scheme : rec->negotiated_scheme;
    auto* ops = find_connector(scheme);
    if (!ops) {
        LOG_WARN("send_frame #{}: no connector for '{}'", id, scheme);
        return;
    }

    // Queue into per-conn send queue, then flush one batch
    auto q = get_or_create_queue(id);
    if (!q->try_push(std::move(frame))) {
        bus_.emit_drop(id, DropReason::PerConnLimitExceeded);
        LOG_WARN("send_frame #{}: per-conn queue full", id);
        return;
    }
    flush_queue(id, *q);
}

void ConnectionManager::flush_queue(conn_id_t id, PerConnQueue& q) {
    auto rec = rcu_find(id);
    if (!rec) return;
    const std::string scheme = rec->negotiated_scheme.empty()
                             ? rec->local_scheme : rec->negotiated_scheme;
    auto* ops = find_connector(scheme);
    if (!ops) return;

    auto batch = q.drain_batch(64);
    if (batch.empty()) return;
    flush_frames_to_connector(id, ops, batch);
}

// ── send ─────────────────────────────────────────────────────────────────────

bool ConnectionManager::send(const char* uri, uint32_t msg_type,
                              std::span<const uint8_t> payload) {
    if (!uri || shutting_down_.load(std::memory_order_relaxed)) return false;

    auto conn_id_opt = resolve_uri(std::string_view(uri));
    if (!conn_id_opt) {
        const std::string uri_str(uri);
        const auto sep = uri_str.find("://");
        const std::string scheme = (sep != std::string::npos)
                                 ? uri_str.substr(0, sep) : "tcp";
        if (auto* ops = find_connector(scheme))
            ops->connect(ops->connector_ctx, uri_str.c_str());
        return false;
    }

    const conn_id_t id = *conn_id_opt;
    auto rec = rcu_find(id);
    if (!rec) return false;

    const bool is_system = (msg_type == MSG_TYPE_AUTH || msg_type == MSG_TYPE_KEY_EXCHANGE);
    if (rec->state != STATE_ESTABLISHED && !is_system) return false;

    // Check global in-flight limit
    const size_t global_pending = get_pending_bytes();
    if (global_pending + payload.size() > GLOBAL_MAX_IN_FLIGHT) {
        bus_.emit_stat({StatsEvent::Kind::Backpressure, 1, id});
        return false;
    }

    if (payload.size() > CHUNK_SIZE * 2) {
        size_t offset = 0;
        while (offset < payload.size()) {
            const size_t chunk = std::min(CHUNK_SIZE, payload.size() - offset);
            send_frame(id, msg_type, payload.subspan(offset, chunk));
            offset += chunk;
        }
    } else {
        send_frame(id, msg_type, payload);
    }
    return true;
}

bool ConnectionManager::send_on_conn(conn_id_t id, uint32_t msg_type,
                                      std::span<const uint8_t> payload) {
    if (!rcu_find(id)) return false;
    send_frame(id, msg_type, payload);
    return true;
}

void ConnectionManager::broadcast(uint32_t msg_type,
                                   std::span<const uint8_t> payload) {
    auto map = rcu_read();
    for (auto& [id, rec] : *map) {
        if (rec->state == STATE_ESTABLISHED)
            send_frame(id, msg_type, payload);
    }
}

} // namespace gn