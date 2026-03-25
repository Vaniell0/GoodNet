/// @file core/cm/dispatch.cpp
/// Packet reassembly, dispatch, heartbeat keepalive.

#include "impl.hpp"
#include "logger.hpp"

#include <chrono>
#include <cstring>

namespace gn {

// ═══════════════════════════════════════════════════════════════════════════════
// handle_data — reassembly + fast-path
// ═══════════════════════════════════════════════════════════════════════════════

void ConnectionManager::Impl::handle_data(conn_id_t id, const void* raw, size_t size) {
    if (shutting_down_.load(std::memory_order_relaxed)) return;
    LOG_TRACE("handle_data #{}: {} bytes", id, size);
    const uint64_t recv_ts = monotonic_ns();

    // Resolve transport_conn_id → peer conn_id (для вторичных транспортов)
    conn_id_t peer_id = id;
    {
        std::shared_lock lk(transport_mu_);
        auto it = transport_index_.find(id);
        if (it != transport_index_.end()) peer_id = it->second;
    }

    std::shared_ptr<ConnectionRecord> rec;
    {
        auto map = rcu_read();
        auto it  = map->find(peer_id);
        if (it == map->end()) return;
        rec = it->second;
    }

    // Fast path: complete frame, no buffered residue — zero-copy dispatch.
    if (rec->recv_buf.empty() && size >= sizeof(header_t)) {
        const auto* hdr = reinterpret_cast<const header_t*>(raw);
        const size_t total = sizeof(header_t) + hdr->payload_len;

        if (size == total
            && hdr->magic == GNET_MAGIC
            && hdr->proto_ver == GNET_PROTO_VER)
        {
            LOG_TRACE("handle_data #{}: fast-path type={}", peer_id, hdr->payload_type);
            const std::span<const uint8_t> payload(
                static_cast<const uint8_t*>(raw) + sizeof(header_t),
                hdr->payload_len);
            dispatch_packet(peer_id, hdr, payload, recv_ts);
            return;
        }
    }

    {
        auto& buf = rec->recv_buf;
        const auto* bytes = static_cast<const uint8_t*>(raw);
        buf.insert(buf.end(), bytes, bytes + size);
        LOG_TRACE("handle_data #{}: reassembly, buf={} bytes", peer_id, buf.size());

        // M2 fix: recv_buf unbounded growth protection
        if (buf.size() > MAX_RECV_BUF) {
            LOG_WARN("handle_data #{}: recv_buf overflow ({} bytes) — closing",
                     peer_id, buf.size());
            bus_.emit_drop(peer_id, DropReason::RecvBufOverflow);
            close_now(peer_id);
            return;
        }
    }

    thread_local std::vector<uint8_t> pkt_buf;

    size_t consumed = 0;
    while (true) {
        auto& buf   = rec->recv_buf;
        const size_t avail = buf.size() - consumed;

        if (avail < sizeof(header_t)) break;

        const auto* hdr = reinterpret_cast<const header_t*>(buf.data() + consumed);

        if (hdr->magic != GNET_MAGIC) {
            LOG_WARN("handle_data #{}: bad magic 0x{:08X} — closing",
                     peer_id, hdr->magic);
            bus_.emit_drop(peer_id, DropReason::BadMagic);
            close_now(peer_id);
            return;
        }
        if (hdr->proto_ver != GNET_PROTO_VER) {
            LOG_WARN("handle_data #{}: bad proto_ver {} — closing",
                     peer_id, hdr->proto_ver);
            bus_.emit_drop(peer_id, DropReason::BadProtoVer);
            close_now(peer_id);
            return;
        }

        const size_t total = sizeof(header_t) + hdr->payload_len;
        if (avail < total) break;

        pkt_buf.assign(buf.data() + consumed, buf.data() + consumed + total);
        consumed += total;

        const auto* phdr    = reinterpret_cast<const header_t*>(pkt_buf.data());
        const std::span<const uint8_t> payload(
            pkt_buf.data() + sizeof(header_t), phdr->payload_len);

        dispatch_packet(peer_id, phdr, payload, recv_ts);

        // Re-acquire after potential state changes inside dispatch
        auto map = rcu_read();
        auto it  = map->find(peer_id);
        if (it == map->end()) return;
        rec = it->second;
    }

    if (consumed > 0) {
        auto& buf = rec->recv_buf;
        if (consumed == buf.size()) buf.clear();
        else buf.erase(buf.begin(), buf.begin() + static_cast<ptrdiff_t>(consumed));
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// dispatch_packet
// ═══════════════════════════════════════════════════════════════════════════════

void ConnectionManager::Impl::dispatch_packet(conn_id_t id, const header_t* hdr,
                                               std::span<const uint8_t> payload,
                                               uint64_t recv_ts_ns) {
    // M5 fix: track in-flight dispatches for clean shutdown
    in_flight_dispatches_.fetch_add(1, std::memory_order_relaxed);
    struct DispatchGuard {
        std::atomic<uint32_t>& counter;
        ~DispatchGuard() { counter.fetch_sub(1, std::memory_order_release); }
    } guard{in_flight_dispatches_};
    LOG_TRACE("dispatch #{}: type={} payload={}", id, hdr->payload_type, payload.size());

    // ── Noise handshake messages ─────────────────────────────────────────────
    if (hdr->payload_type == MSG_TYPE_NOISE_INIT) {
        handle_noise_init(id, payload);
        return;
    }
    if (hdr->payload_type == MSG_TYPE_NOISE_RESP) {
        handle_noise_resp(id, payload);
        return;
    }
    if (hdr->payload_type == MSG_TYPE_NOISE_FIN) {
        handle_noise_fin(id, payload);
        return;
    }

    // ── Normal dispatch ──────────────────────────────────────────────────────

    auto rec = rcu_find(id);
    if (!rec) { bus_.emit_drop(id, DropReason::ConnNotFound); return; }

    if (rec->state != STATE_ESTABLISHED) {
        LOG_WARN("dispatch #{}: type={} before ESTABLISHED", id, hdr->payload_type);
        bus_.emit_drop(id, DropReason::StateNotEstablished);
        return;
    }

    // ── Localhost fast-path: без decrypt/decompress ──────────────────────────
    if (rec->localhost_passthrough) {
        bus_.emit_stat({StatsEvent::Kind::RxBytes,  payload.size(), id});
        bus_.emit_stat({StatsEvent::Kind::RxPacket, 1,              id});

        if (hdr->payload_type == MSG_TYPE_HEARTBEAT) {
            handle_heartbeat(id, payload);
            return;
        }
        if (hdr->payload_type == MSG_TYPE_RELAY) {
            handle_relay(id, payload);
            return;
        }

        auto hdr_ptr  = std::make_shared<header_t>(*hdr);
        auto data_ptr = std::make_shared<sdk::RawBuffer>(
            std::vector<uint8_t>(payload.begin(), payload.end()));

        endpoint_t remote  = rec->remote;
        remote.peer_id     = id;
        const auto affinity = rec->affinity_plugin;

        const auto result = bus_.dispatch_packet(
            hdr->payload_type, hdr_ptr, &remote, data_ptr);

        const uint64_t lat_ns = monotonic_ns() - recv_ts_ns;
        bus_.emit_latency(id, lat_ns);

        if (result.result == PROPAGATION_CONSUMED && affinity.empty()) {
            std::lock_guard wlk(records_write_mu_);
            rcu_update([&](RecordMap& m) {
                if (auto it = m.find(id); it != m.end())
                    it->second->affinity_plugin = result.consumed_by;
            });
            LOG_DEBUG("dispatch #{}: affinity → '{}'", id, result.consumed_by);
            bus_.emit_stat({StatsEvent::Kind::Consumed, 1, id});
        } else if (result.result == PROPAGATION_REJECT) {
            LOG_WARN("dispatch #{}: REJECTED by '{}' (type={})",
                     id, result.consumed_by, hdr->payload_type);
            bus_.emit_drop(id, DropReason::RejectedByHandler);
            bus_.emit_stat({StatsEvent::Kind::Rejected, 1, id});
        }
        return;
    }

    // ── Standard path ────────────────────────────────────────────────────────

    std::vector<uint8_t> plaintext;
    if (hdr->flags & GNET_FLAG_TRUSTED) {
        if (!rec->is_localhost) {
            LOG_WARN("dispatch #{}: TRUSTED flag from non-localhost — dropping", id);
            bus_.emit_drop(id, DropReason::TrustedFromRemote);
            return;
        }
        plaintext.assign(payload.begin(), payload.end());
    } else if (rec->is_localhost || !rec->session) {
        plaintext.assign(payload.begin(), payload.end());
    } else {
        plaintext = rec->session->decrypt(payload.data(), payload.size(),
                                           hdr->packet_id);
        LOG_TRACE("dispatch #{}: decrypted {} → {} bytes",
                  id, payload.size(), plaintext.size());
        if (plaintext.empty()) {
            bus_.emit_drop(id, DropReason::DecryptFail);
            return;
        }
    }

    bus_.emit_stat({StatsEvent::Kind::RxBytes,  payload.size(), id});
    bus_.emit_stat({StatsEvent::Kind::RxPacket, 1,              id});

    if (hdr->payload_type == MSG_TYPE_HEARTBEAT) {
        handle_heartbeat(id, std::span<const uint8_t>(plaintext));
        return;
    }

    if (hdr->payload_type == MSG_TYPE_RELAY) {
        handle_relay(id, std::span<const uint8_t>(plaintext));
        return;
    }

    auto hdr_ptr  = std::make_shared<header_t>(*hdr);
    auto data_ptr = std::make_shared<sdk::RawBuffer>(std::move(plaintext));

    endpoint_t remote  = rec->remote;
    remote.peer_id     = id;
    const auto affinity = rec->affinity_plugin;

    const auto result = bus_.dispatch_packet(
        hdr->payload_type, hdr_ptr, &remote, data_ptr);

    const uint64_t lat_ns = monotonic_ns() - recv_ts_ns;
    bus_.emit_latency(id, lat_ns);

    if (result.result == PROPAGATION_CONSUMED && affinity.empty()) {
        std::lock_guard wlk(records_write_mu_);
        rcu_update([&](RecordMap& m) {
            if (auto it = m.find(id); it != m.end())
                it->second->affinity_plugin = result.consumed_by;
        });
        LOG_DEBUG("dispatch #{}: affinity → '{}'", id, result.consumed_by);
        bus_.emit_stat({StatsEvent::Kind::Consumed, 1, id});
    } else if (result.result == PROPAGATION_REJECT) {
        LOG_WARN("dispatch #{}: REJECTED by '{}' (type={})",
                 id, result.consumed_by, hdr->payload_type);
        bus_.emit_drop(id, DropReason::RejectedByHandler);
        bus_.emit_stat({StatsEvent::Kind::Rejected, 1, id});
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Heartbeat
// ═══════════════════════════════════════════════════════════════════════════════

void ConnectionManager::Impl::send_heartbeat(conn_id_t id) {
    auto rec = rcu_find(id);
    if (!rec || rec->state != STATE_ESTABLISHED) return;

    msg::HeartbeatPayload hb{};
    hb.timestamp_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    hb.seq   = rec->heartbeat_seq.fetch_add(1, std::memory_order_relaxed);
    hb.flags = 0x00;  // PING

    // Собираем transport info extension
    const auto path_count = static_cast<uint8_t>(
        std::min(rec->transport_paths.size(),
                 static_cast<size_t>(msg::HEARTBEAT_MAX_PATHS)));

    const size_t ext_size = sizeof(msg::HeartbeatTransportInfo)
                          + path_count * sizeof(msg::HeartbeatPathEntry);
    std::vector<uint8_t> buf(sizeof(hb) + ext_size);
    std::memcpy(buf.data(), &hb, sizeof(hb));

    auto* info = reinterpret_cast<msg::HeartbeatTransportInfo*>(
        buf.data() + sizeof(hb));
    info->path_count = path_count;

    auto* entries = reinterpret_cast<msg::HeartbeatPathEntry*>(
        buf.data() + sizeof(hb) + sizeof(msg::HeartbeatTransportInfo));
    for (uint8_t i = 0; i < path_count; ++i) {
        const auto& tp = rec->transport_paths[i];
        std::memset(&entries[i], 0, sizeof(msg::HeartbeatPathEntry));
        std::strncpy(entries[i].scheme, tp.scheme.c_str(),
                     sizeof(entries[i].scheme) - 1);
        entries[i].active         = tp.active ? 1 : 0;
        entries[i].priority       = tp.priority;
        entries[i].rtt_compressed = static_cast<uint16_t>(
            std::min(tp.last_rtt_us / 10, uint64_t(65535)));
    }

    LOG_TRACE("send_heartbeat #{}: seq={} paths={}", id, hb.seq, path_count);
    send_frame(id, MSG_TYPE_HEARTBEAT, std::span<const uint8_t>(buf));
}

void ConnectionManager::Impl::handle_heartbeat(conn_id_t id,
                                                std::span<const uint8_t> payload) {
    if (payload.size() < sizeof(msg::HeartbeatPayload)) return;
    const auto* hb = reinterpret_cast<const msg::HeartbeatPayload*>(payload.data());

    auto rec = rcu_find(id);
    if (!rec) return;

    // Парсим transport info extension (если есть)
    const size_t ext_offset = sizeof(msg::HeartbeatPayload);
    if (payload.size() > ext_offset + sizeof(msg::HeartbeatTransportInfo)) {
        const auto* info = reinterpret_cast<const msg::HeartbeatTransportInfo*>(
            payload.data() + ext_offset);
        const size_t entries_offset = ext_offset + sizeof(msg::HeartbeatTransportInfo);
        const size_t available = payload.size() - entries_offset;
        const uint8_t count = std::min(info->path_count, msg::HEARTBEAT_MAX_PATHS);
        const uint8_t parseable = static_cast<uint8_t>(
            std::min(static_cast<size_t>(count),
                     available / sizeof(msg::HeartbeatPathEntry)));

        if (parseable > 0) {
            const auto* entries = reinterpret_cast<const msg::HeartbeatPathEntry*>(
                payload.data() + entries_offset);
            std::vector<ConnectionRecord::PeerPathInfo> peer_info;
            peer_info.reserve(parseable);
            for (uint8_t i = 0; i < parseable; ++i) {
                ConnectionRecord::PeerPathInfo pi;
                pi.scheme         = std::string(entries[i].scheme,
                    strnlen(entries[i].scheme, sizeof(entries[i].scheme)));
                pi.active         = entries[i].active != 0;
                pi.priority       = entries[i].priority;
                pi.rtt_compressed = entries[i].rtt_compressed;
                peer_info.push_back(std::move(pi));
            }
            rec->peer_transport_info = std::move(peer_info);
        }
    }

    if (hb->flags == 0x00) {
        // PING → respond with PONG
        msg::HeartbeatPayload pong{};
        pong.timestamp_us = hb->timestamp_us;
        pong.seq          = hb->seq;
        pong.flags        = 0x01;  // PONG

        send_frame(id, MSG_TYPE_HEARTBEAT,
                   std::span<const uint8_t>(
                       reinterpret_cast<const uint8_t*>(&pong), sizeof(pong)));
    } else if (hb->flags == 0x01) {
        // PONG → update last_heartbeat_recv, reset missed counter
        rec->last_heartbeat_recv.store(
            std::chrono::steady_clock::now().time_since_epoch().count(),
            std::memory_order_release);
        rec->missed_heartbeats.store(0, std::memory_order_release);

        // RTT измерение: timestamp_us из PING возвращается в PONG
        const uint64_t now_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        if (now_us > hb->timestamp_us) {
            const uint64_t rtt_us = now_us - hb->timestamp_us;
            LOG_TRACE("heartbeat #{}: PONG seq={} rtt={}us", id, hb->seq, rtt_us);
            auto* path = rec->best_path();
            if (path) path->last_rtt_us = rtt_us;
        }
    }
}

void ConnectionManager::Impl::check_heartbeat_timeouts() {
    const auto now = std::chrono::steady_clock::now();

    auto map = rcu_read();
    for (auto& [cid, rec] : *map) {
        if (rec->state != STATE_ESTABLISHED) continue;

        const auto last_ns = rec->last_heartbeat_recv.load(std::memory_order_acquire);
        if (last_ns == 0) {
            // First heartbeat cycle — initialize
            rec->last_heartbeat_recv.store(
                now.time_since_epoch().count(), std::memory_order_release);
            continue;
        }

        const auto last_tp = std::chrono::steady_clock::time_point(
            std::chrono::steady_clock::duration(last_ns));
        const auto elapsed = now - last_tp;

        if (elapsed > HEARTBEAT_INTERVAL) {
            const auto missed = rec->missed_heartbeats.fetch_add(1, std::memory_order_relaxed) + 1;
            if (missed >= MAX_MISSED_HEARTBEATS) {
                LOG_WARN("Heartbeat #{}: {} missed → disconnecting", cid, missed);
                disconnect(cid);
            } else {
                send_heartbeat(cid);
            }
        }
    }
}

} // namespace gn
