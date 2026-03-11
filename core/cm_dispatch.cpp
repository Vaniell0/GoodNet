#include "connectionManager.hpp"
#include "logger.hpp"
#include <cstring>

namespace gn {

// ─── handle_data ─────────────────────────────────────────────────────────────

void ConnectionManager::handle_data(conn_id_t id, const void* raw, size_t size) {
    if (shutting_down_.load(std::memory_order_relaxed)) return;

    std::unique_lock lock(records_mu_);
    auto it = records_.find(id);
    if (it == records_.end()) return;

    auto& rec = it->second;
    const auto* bytes = static_cast<const uint8_t*>(raw);
    rec.recv_buf.insert(rec.recv_buf.end(), bytes, bytes + size);

    while (rec.recv_buf.size() >= sizeof(header_t)) {
        const auto* hdr = reinterpret_cast<const header_t*>(rec.recv_buf.data());

        if (hdr->magic != GNET_MAGIC) {
            LOG_WARN("handle_data #{}: bad magic 0x{:08X}, dropping", id, hdr->magic);
            rec.recv_buf.clear(); break;
        }
        if (hdr->proto_ver != GNET_PROTO_VER) {
            LOG_WARN("handle_data #{}: bad proto_ver {}", id, hdr->proto_ver);
            rec.recv_buf.clear(); break;
        }

        const size_t total = sizeof(header_t) + hdr->payload_len;
        if (rec.recv_buf.size() < total) break;

        std::vector<uint8_t> pkt(rec.recv_buf.begin(),
                                  rec.recv_buf.begin() + static_cast<ptrdiff_t>(total));
        rec.recv_buf.erase(rec.recv_buf.begin(),
                           rec.recv_buf.begin() + static_cast<ptrdiff_t>(total));

        const auto* phdr    = reinterpret_cast<const header_t*>(pkt.data());
        const uint8_t* payload = pkt.data() + sizeof(header_t);
        const size_t plen   = phdr->payload_len;

        lock.unlock();
        dispatch_packet(id, phdr, payload, plen);
        lock.lock();

        it = records_.find(id);
        if (it == records_.end()) break;
    }
}

// ─── dispatch_packet ─────────────────────────────────────────────────────────

void ConnectionManager::dispatch_packet(conn_id_t id, const header_t* hdr,
                                         const uint8_t* payload, size_t plen) {
    if (hdr->payload_type == MSG_TYPE_AUTH) {
        const bool ok = process_auth(id, payload, plen);
        if (ok)   stats_.auth_ok  .fetch_add(1, std::memory_order_relaxed);
        else      stats_.auth_fail.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    endpoint_t remote{};
    bool       is_localhost = false;
    std::string affinity_hint;
    std::vector<uint8_t> plaintext;

    {
        std::unique_lock lk(records_mu_);
        auto it = records_.find(id);
        if (it == records_.end()) return;
        auto& rec = it->second;

        if (rec.state != STATE_ESTABLISHED) {
            LOG_WARN("dispatch #{}: type={} before ESTABLISHED, dropping",
                     id, hdr->payload_type);
            return;
        }

        remote         = rec.remote;
        remote.peer_id = id;          // lets handlers call send_response(ep->peer_id)
        is_localhost   = rec.is_localhost;
        affinity_hint  = rec.affinity_plugin;

        if (is_localhost || !rec.session) {
            plaintext.assign(payload, payload + plen);
        } else if (rec.session->ready) {
            plaintext = rec.session->decrypt(payload, plen);
            if (plaintext.empty()) {
                LOG_WARN("dispatch #{}: decrypt failed (type={}), dropping",
                         id, hdr->payload_type);
                stats_.decrypt_fail.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        } else {
            LOG_WARN("dispatch #{}: session not ready, dropping type={}", id, hdr->payload_type);
            return;
        }
    }

    stats_.on_rx(plen);

    auto hdr_ptr  = std::make_shared<header_t>(*hdr);
    auto data_ptr = std::make_shared<std::vector<uint8_t>>(std::move(plaintext));

    // Pipeline dispatch — replaces old bus_.emit()
    const auto result = bus_.dispatch_packet(
        hdr->payload_type, hdr_ptr, &remote, data_ptr);

    // Session affinity: pin on first CONSUMED
    if (result.result == PROPAGATION_CONSUMED && affinity_hint.empty()) {
        std::unique_lock lk(records_mu_);
        if (auto it = records_.find(id); it != records_.end())
            it->second.affinity_plugin = result.consumed_by;
        LOG_DEBUG("dispatch #{}: affinity pinned → '{}'", id, result.consumed_by);
        stats_.consumed.fetch_add(1, std::memory_order_relaxed);
    } else if (result.result == PROPAGATION_REJECT) {
        LOG_WARN("dispatch #{}: REJECTED by '{}' (type={})",
                 id, result.consumed_by, hdr->payload_type);
        stats_.rejected.fetch_add(1, std::memory_order_relaxed);
    }
}

// ─── send_on_conn ─────────────────────────────────────────────────────────────
// Used by host_api_t::send_response — direct reply without URI lookup.

void ConnectionManager::send_on_conn(conn_id_t id, uint32_t msg_type,
                                      const void* data, size_t size) {
    {
        std::shared_lock lk(records_mu_);
        if (records_.find(id) == records_.end()) return;
    }
    send_frame(id, msg_type, data, size);
}

} // namespace gn
