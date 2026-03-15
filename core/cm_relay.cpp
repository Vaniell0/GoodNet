/// @file core/cm_relay.cpp
/// Gossip relay: handle_relay(), relay(), relay_seen().

#include "connectionManager.hpp"
#include "logger.hpp"

#include <cstring>

namespace gn {

// ── relay_seen ────────────────────────────────────────────────────────────────
/// Ring-buffer dedup: returns true if (sender_id, packet_id) was already seen.

bool ConnectionManager::relay_seen(const header_t* inner_hdr) {
    // Hash sender_id[16] into a uint64_t for fast comparison.
    uint64_t sh = 0;
    std::memcpy(&sh, inner_hdr->sender_id, sizeof(sh));
    // XOR second half for better distribution.
    uint64_t sh2 = 0;
    std::memcpy(&sh2, inner_hdr->sender_id + 8, sizeof(sh2));
    sh ^= sh2;

    const uint64_t pid = inner_hdr->packet_id;

    std::lock_guard lock(relay_dedup_mu_);
    for (size_t i = 0; i < RELAY_DEDUP_SIZE; ++i) {
        if (relay_dedup_[i].sender_hash == sh &&
            relay_dedup_[i].packet_id   == pid)
            return true;
    }
    // Not seen — insert at ring head.
    relay_dedup_[relay_dedup_pos_] = {sh, pid};
    relay_dedup_pos_ = (relay_dedup_pos_ + 1) % RELAY_DEDUP_SIZE;
    return false;
}

// ── handle_relay ──────────────────────────────────────────────────────────────
/// Called from dispatch_packet after decryption when payload_type == MSG_TYPE_RELAY.
///
/// plaintext layout: RelayPayload(33) + inner_frame(variable)

void ConnectionManager::handle_relay(conn_id_t id,
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
        if (std::memcmp(rp->dest_pubkey, identity_.user_pubkey, 32) == 0) {
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
/// Public API: wrap inner_frame in RelayPayload and send to all ESTABLISHED
/// peers except exclude_conn.

void ConnectionManager::relay(conn_id_t exclude_conn, uint8_t ttl,
                               const uint8_t dest_pubkey[32],
                               std::span<const uint8_t> inner_frame) {
    // Build relay payload: RelayPayload header + inner_frame.
    constexpr size_t RELAY_HDR = sizeof(msg::RelayPayload);
    std::vector<uint8_t> relay_payload(RELAY_HDR + inner_frame.size());

    auto* rp = reinterpret_cast<msg::RelayPayload*>(relay_payload.data());
    rp->ttl = ttl;
    std::memcpy(rp->dest_pubkey, dest_pubkey, 32);
    std::memcpy(relay_payload.data() + RELAY_HDR,
                inner_frame.data(), inner_frame.size());

    // Send to all ESTABLISHED peers except exclude_conn.
    auto map = rcu_read();
    for (auto& [cid, rec] : *map) {
        if (cid == exclude_conn) continue;
        if (rec->state != STATE_ESTABLISHED) continue;
        send_frame(cid, MSG_TYPE_RELAY,
                   std::span<const uint8_t>(relay_payload));
    }
}

} // namespace gn