/// @file core/cm_query.cpp
/// Read-only query methods.

#include "connectionManager.hpp"

namespace gn {

std::string bytes_to_hex(const uint8_t* data, size_t len);

// ── local_core_meta / find_conn_by_pubkey ────────────────────────────────────

msg::CoreMeta ConnectionManager::local_core_meta() const {
    msg::CoreMeta m{};
    m.core_version = GN_CORE_VERSION;
    m.caps_mask    = CORE_CAP_ZSTD | CORE_CAP_KEYROT | CORE_CAP_RELAY;
    {
        std::shared_lock lk(connectors_mu_);
        if (connectors_.count("ice"))
            m.caps_mask |= CORE_CAP_ICE;
    }
    return m;
}

conn_id_t ConnectionManager::find_conn_by_pubkey(const char* pubkey_hex) const {
    if (!pubkey_hex) return CONN_ID_INVALID;
    std::shared_lock lk(pk_mu_);
    auto it = pk_index_.find(pubkey_hex);
    return it != pk_index_.end() ? it->second : CONN_ID_INVALID;
}

// ── Queries ───────────────────────────────────────────────────────────────────

size_t ConnectionManager::connection_count() const {
    return rcu_read()->size();
}
std::vector<std::string> ConnectionManager::get_active_uris() const {
    std::shared_lock lk(uri_mu_);
    std::vector<std::string> out; out.reserve(uri_index_.size());
    for (auto& [uri, _] : uri_index_) out.push_back(uri);
    return out;
}
std::vector<conn_id_t> ConnectionManager::get_active_conn_ids() const {
    auto map = rcu_read();
    std::vector<conn_id_t> out;
    out.reserve(map->size());
    for (auto& [id, rec] : *map)
        if (rec->state == STATE_ESTABLISHED) out.push_back(id);
    return out;
}
std::optional<conn_state_t> ConnectionManager::get_state(conn_id_t id) const {
    auto rec = rcu_find(id);
    if (rec) return rec->state;
    return std::nullopt;
}
std::optional<std::string> ConnectionManager::get_negotiated_scheme(conn_id_t id) const {
    auto rec = rcu_find(id);
    if (rec) return rec->negotiated_scheme;
    return std::nullopt;
}
std::optional<std::vector<uint8_t>> ConnectionManager::get_peer_pubkey(conn_id_t id) const {
    auto rec = rcu_find(id);
    if (rec && rec->peer_authenticated)
        return std::vector<uint8_t>(rec->peer_user_pubkey,
                                    rec->peer_user_pubkey + 32);
    return std::nullopt;
}
bool ConnectionManager::get_peer_endpoint(conn_id_t id, endpoint_t& out) const {
    auto rec = rcu_find(id);
    if (!rec) return false;
    out = rec->remote;
    out.peer_id = id;
    return true;
}
size_t ConnectionManager::get_pending_bytes(conn_id_t id) const noexcept {
    if (id == CONN_ID_INVALID) {
        size_t total = 0;
        std::shared_lock lk(queues_mu_);
        for (auto& [_, q] : send_queues_)
            total += q->pending_bytes.load(std::memory_order_relaxed);
        return total;
    }
    std::shared_lock lk(queues_mu_);
    auto it = send_queues_.find(id);
    return it != send_queues_.end()
        ? it->second->pending_bytes.load(std::memory_order_relaxed) : 0;
}

} // namespace gn
