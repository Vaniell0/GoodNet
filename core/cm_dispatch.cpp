/// @file core/cm_dispatch.cpp

#include "connectionManager.hpp"
#include "logger.hpp"

#include <cstring>
#include <ctime>

namespace gn {

// ── handle_data ───────────────────────────────────────────────────────────────

void ConnectionManager::handle_data(conn_id_t id, const void* raw, size_t size) {
    if (shutting_down_.load(std::memory_order_relaxed)) return;
    const uint64_t recv_ts = monotonic_ns();

    // We need a write-capable view of recv_buf — use a direct record access
    // via write-locked RCU to append bytes (recv_buf is hot, per-connection).
    std::shared_ptr<ConnectionRecord> rec;
    {
        auto map = rcu_read();
        auto it  = map->find(id);
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
            const std::span<const uint8_t> payload(
                static_cast<const uint8_t*>(raw) + sizeof(header_t),
                hdr->payload_len);
            dispatch_packet(id, hdr, payload, recv_ts);
            return;
        }
    }

    {
        auto& buf = rec->recv_buf;
        const auto* bytes = static_cast<const uint8_t*>(raw);
        buf.insert(buf.end(), bytes, bytes + size);
    }

    thread_local std::vector<uint8_t> pkt_buf;

    size_t consumed = 0;
    while (true) {
        auto& buf   = rec->recv_buf;
        const size_t avail = buf.size() - consumed;

        if (avail < sizeof(header_t)) break;

        const auto* hdr = reinterpret_cast<const header_t*>(buf.data() + consumed);

        if (hdr->magic != GNET_MAGIC) {
            LOG_WARN("handle_data #{}: bad magic 0x{:08X}", id, hdr->magic);
            bus_.emit_drop(id, DropReason::BadMagic);
            buf.clear(); consumed = 0; break;
        }
        if (hdr->proto_ver != GNET_PROTO_VER) {
            LOG_WARN("handle_data #{}: bad proto_ver {}", id, hdr->proto_ver);
            bus_.emit_drop(id, DropReason::BadProtoVer);
            buf.clear(); consumed = 0; break;
        }

        const size_t total = sizeof(header_t) + hdr->payload_len;
        if (avail < total) break;

        pkt_buf.assign(buf.data() + consumed, buf.data() + consumed + total);
        consumed += total;

        const auto* phdr    = reinterpret_cast<const header_t*>(pkt_buf.data());
        const std::span<const uint8_t> payload(
            pkt_buf.data() + sizeof(header_t), phdr->payload_len);

        dispatch_packet(id, phdr, payload, recv_ts);

        // Re-acquire after potential state changes inside dispatch
        auto map = rcu_read();
        auto it  = map->find(id);
        if (it == map->end()) return;
        rec = it->second;
    }

    if (consumed > 0) {
        auto& buf = rec->recv_buf;
        if (consumed == buf.size()) buf.clear();
        else buf.erase(buf.begin(), buf.begin() + static_cast<ptrdiff_t>(consumed));
    }
}

// ── dispatch_packet ───────────────────────────────────────────────────────────

void ConnectionManager::dispatch_packet(conn_id_t id, const header_t* hdr,
                                         std::span<const uint8_t> payload,
                                         uint64_t recv_ts_ns) {
    // ── AUTH ──────────────────────────────────────────────────────────────────
    if (hdr->payload_type == MSG_TYPE_AUTH) {
        const bool ok = process_auth(id, payload);
        bus_.emit_stat({ok ? StatsEvent::Kind::AuthOk : StatsEvent::Kind::AuthFail,
                        1, id});
        return;
    }

    // ── KEY_EXCHANGE ──────────────────────────────────────────────────────────
    if (hdr->payload_type == MSG_TYPE_KEY_EXCHANGE) {
        process_keyex(id, payload);
        return;
    }

    // ── ICE_SIGNAL ────────────────────────────────────────────────────────────
    if (hdr->payload_type == MSG_TYPE_ICE_SIGNAL) {
        handle_ice_signal(id, payload);
        return;
    }

    // ── Normal dispatch ───────────────────────────────────────────────────────

    auto rec = rcu_find(id);
    if (!rec) { bus_.emit_drop(id, DropReason::ConnNotFound); return; }

    if (rec->state != STATE_ESTABLISHED) {
        LOG_WARN("dispatch #{}: type={} before ESTABLISHED", id, hdr->payload_type);
        bus_.emit_drop(id, DropReason::StateNotEstablished);
        return;
    }

    // Decrypt
    std::vector<uint8_t> plaintext;
    if (rec->is_localhost || !rec->session) {
        plaintext.assign(payload.begin(), payload.end());
    } else if (rec->session->ready) {
        plaintext = rec->session->decrypt(payload.data(), payload.size());
        if (plaintext.empty()) {
            bus_.emit_drop(id, DropReason::DecryptFail);
            return;
        }
    } else {
        bus_.emit_drop(id, DropReason::SessionNotReady);
        return;
    }

    bus_.emit_stat({StatsEvent::Kind::RxBytes,  payload.size(), id});
    bus_.emit_stat({StatsEvent::Kind::RxPacket, 1,              id});

    auto hdr_ptr  = std::make_shared<header_t>(*hdr);
    auto data_ptr = std::make_shared<sdk::RawBuffer>(std::move(plaintext));

    endpoint_t remote  = rec->remote;
    remote.peer_id     = id;
    const auto affinity = rec->affinity_plugin;

    const auto result = bus_.dispatch_packet(
        hdr->payload_type, hdr_ptr, &remote, data_ptr);

    // Latency: nanoseconds from byte arrival to handler return
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

} // namespace gn
