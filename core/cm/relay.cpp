/// @file core/cm/relay.cpp
/// Smart relay: handle_relay(), relay(), relay_seen().
/// Gossip broadcast with O(1) dedup hash set.

#include "impl.hpp"
#include "logger.hpp"

#include <chrono>
#include <cstring>
#include <sodium/crypto_sign.h>

#include "util.hpp"

namespace gn {

// ── relay_seen ────────────────────────────────────────────────────────────────
/// Hash-set dedup using packet_id only (header v3 has no sender_id).

bool ConnectionManager::Impl::relay_seen(const header_t* inner_hdr) {
    const uint64_t pid = inner_hdr->packet_id;
    const auto now = std::chrono::steady_clock::now();

    // Используем packet_id + payload_type как ключ дедупликации
    uint64_t sh = std::hash<uint64_t>{}(pid) ^ (static_cast<uint64_t>(inner_hdr->payload_type) << 32);

    std::lock_guard lock(relay_dedup_mu_);

    // Periodic TTL eviction
    if (relay_dedup_set_.size() > 8192) {
        const auto cutoff = now - std::chrono::seconds(30);
        const size_t before = relay_dedup_set_.size();
        for (auto it = relay_dedup_set_.begin(); it != relay_dedup_set_.end(); ) {
            if (it->ts < cutoff) it = relay_dedup_set_.erase(it);
            else ++it;
        }
        LOG_TRACE("relay_seen: evicted {} stale entries", before - relay_dedup_set_.size());
    }

    RelayFingerprint fp{sh, pid, now};
    if (relay_dedup_set_.count(fp))
        return true;

    relay_dedup_set_.insert(fp);
    return false;
}

// ── handle_relay ──────────────────────────────────────────────────────────────

void ConnectionManager::Impl::handle_relay(conn_id_t id,
                                             std::span<const uint8_t> plaintext) {
    constexpr size_t RELAY_HDR = sizeof(msg::RelayPayload); // 33

    if (plaintext.size() < RELAY_HDR + sizeof(header_t)) {
        LOG_WARN("handle_relay #{}: too short ({} bytes)", id, plaintext.size());
        bus_.emit_drop(id, DropReason::RelayDropped);
        return;
    }

    const auto* rp = reinterpret_cast<const msg::RelayPayload*>(plaintext.data());

    if (rp->ttl == 0) {
        LOG_DEBUG("handle_relay #{}: TTL=0, dropping", id);
        bus_.emit_drop(id, DropReason::RelayDropped);
        return;
    }

    const auto inner = plaintext.subspan(RELAY_HDR);
    const auto* inner_hdr = reinterpret_cast<const header_t*>(inner.data());

    // Validate inner frame has enough bytes for its declared payload.
    if (inner.size() < sizeof(header_t) + inner_hdr->payload_len) {
        LOG_WARN("handle_relay #{}: inner frame truncated", id);
        bus_.emit_drop(id, DropReason::RelayDropped);
        return;
    }

    // Dedup check.
    if (relay_seen(inner_hdr)) {
        LOG_DEBUG("handle_relay #{}: dedup hit (pkt_id={})", id, inner_hdr->packet_id);
        return;
    }

    // Check if we are the destination.
    {
        std::shared_lock lk(identity_mu_);
        if (std::memcmp(rp->dest_pubkey, identity_.user_pubkey, crypto_sign_PUBLICKEYBYTES) == 0) {
            // Local delivery: re-enter dispatch_packet with the inner frame.
            const std::span<const uint8_t> inner_payload(
                inner.data() + sizeof(header_t), inner_hdr->payload_len);
            dispatch_packet(id, inner_hdr, inner_payload, monotonic_ns());
            return;
        }
    }

    // Forward to all ESTABLISHED peers except the sender.
    const uint8_t new_ttl = rp->ttl - 1;
    relay(id, new_ttl, rp->dest_pubkey, inner);
}

// ── relay ─────────────────────────────────────────────────────────────────────

void ConnectionManager::Impl::relay(conn_id_t exclude_conn, uint8_t ttl,
                                      const uint8_t dest_pubkey[crypto_sign_PUBLICKEYBYTES],
                                      std::span<const uint8_t> inner_frame) {
    LOG_TRACE("relay: exclude={} ttl={} inner={}", exclude_conn, ttl, inner_frame.size());
    constexpr size_t RELAY_HDR = sizeof(msg::RelayPayload);
    std::vector<uint8_t> relay_payload(RELAY_HDR + inner_frame.size());

    auto* rp = reinterpret_cast<msg::RelayPayload*>(relay_payload.data());
    rp->ttl = ttl;
    std::memcpy(rp->dest_pubkey, dest_pubkey, crypto_sign_PUBLICKEYBYTES);
    std::memcpy(relay_payload.data() + RELAY_HDR,
                inner_frame.data(), inner_frame.size());

    const auto relay_span = std::span<const uint8_t>(relay_payload);

    // Direct connection?
    std::string hex = bytes_to_hex(dest_pubkey, crypto_sign_PUBLICKEYBYTES);
    conn_id_t direct = find_conn_by_pubkey(hex.c_str());
    if (direct != CONN_ID_INVALID && direct != exclude_conn) {
        LOG_TRACE("relay: direct path to {}... via #{}", hex.substr(0, 8), direct);
        send_frame(direct, MSG_TYPE_RELAY, relay_span);
        return;
    }

    // Gossip broadcast
    auto map = rcu_read();
    size_t relay_count = 0;
    for (auto& [cid, rec] : *map) {
        if (cid == exclude_conn) continue;
        if (rec->state != STATE_ESTABLISHED) continue;
        send_frame(cid, MSG_TYPE_RELAY, relay_span);
        ++relay_count;
    }
    LOG_TRACE("relay: gossip to {} peers (exclude=#{})", relay_count, exclude_conn);
}

} // namespace gn
