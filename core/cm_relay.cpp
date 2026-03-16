/// @file core/cm_relay.cpp
/// Smart relay: handle_relay(), relay(), relay_seen().
/// Route table lookup → direct delivery → gossip fallback.

#include "connectionManager.hpp"
#include "route_table.hpp"
#include "logger.hpp"

#include <chrono>
#include <cstring>

namespace gn {

// ── relay_seen ────────────────────────────────────────────────────────────────
/// Hash-set dedup with TTL eviction: O(1) lookup instead of O(N) scan.

bool ConnectionManager::relay_seen(const header_t* inner_hdr) {
    // Hash sender_id[16] into a uint64_t for fast comparison.
    uint64_t sh = 0;
    std::memcpy(&sh, inner_hdr->sender_id, sizeof(sh));
    // XOR second half for better distribution.
    uint64_t sh2 = 0;
    std::memcpy(&sh2, inner_hdr->sender_id + 8, sizeof(sh2));
    sh ^= sh2;

    const uint64_t pid = inner_hdr->packet_id;
    const auto now = std::chrono::steady_clock::now();

    std::lock_guard lock(relay_dedup_mu_);

    // Periodic TTL eviction (every 4096 checks or when set grows large)
    if (relay_dedup_set_.size() > 8192) {
        const auto cutoff = now - std::chrono::seconds(30);
        for (auto it = relay_dedup_set_.begin(); it != relay_dedup_set_.end(); ) {
            if (it->ts < cutoff) it = relay_dedup_set_.erase(it);
            else ++it;
        }
    }

    RelayFingerprint fp{sh, pid, now};
    if (relay_dedup_set_.count(fp))
        return true;

    relay_dedup_set_.insert(fp);
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
/// Smart relay: direct delivery → route table → gossip fallback.

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

    const auto relay_span = std::span<const uint8_t>(relay_payload);

    // Шаг 1: Direct connection? (pk_index_)
    std::string hex = bytes_to_hex(dest_pubkey, 32);
    conn_id_t direct = find_conn_by_pubkey(hex.c_str());
    if (direct != CONN_ID_INVALID && direct != exclude_conn) {
        send_frame(direct, MSG_TYPE_RELAY, relay_span);
        return;
    }

    // Шаг 2: Route table lookup
    if (auto next = route_table_.find_route(dest_pubkey)) {
        if (*next != exclude_conn) {
            send_frame(*next, MSG_TYPE_RELAY, relay_span);
            return;
        }
    }

    // Шаг 3: Fallback — gossip broadcast
    auto map = rcu_read();
    for (auto& [cid, rec] : *map) {
        if (cid == exclude_conn) continue;
        if (rec->state != STATE_ESTABLISHED) continue;
        send_frame(cid, MSG_TYPE_RELAY, relay_span);
    }
}

} // namespace gn