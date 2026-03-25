/// @file core/cm/queries.cpp
/// Read-only getters and diagnostic output for ConnectionManager.

#include "impl.hpp"
#include "logger.hpp"

#include <nlohmann/json.hpp>

#include "util.hpp"

namespace gn {

// =============================================================================
// Single-connection lookups
// =============================================================================

conn_id_t ConnectionManager::Impl::find_conn_by_pubkey(const char* pubkey_hex) const {
    if (!pubkey_hex) return CONN_ID_INVALID;
    std::shared_lock lk(pk_mu_);
    auto it = pk_index_.find(pubkey_hex);
    return it != pk_index_.end() ? it->second : CONN_ID_INVALID;
}

std::optional<conn_state_t> ConnectionManager::Impl::get_state(conn_id_t id) const {
    auto rec = rcu_find(id);
    if (rec) return rec->state;
    return std::nullopt;
}

std::optional<std::string> ConnectionManager::Impl::get_negotiated_scheme(conn_id_t id) const {
    auto rec = rcu_find(id);
    if (rec) return rec->negotiated_scheme;
    return std::nullopt;
}

std::optional<std::vector<uint8_t>> ConnectionManager::Impl::get_peer_pubkey(conn_id_t id) const {
    auto rec = rcu_find(id);
    if (rec && rec->peer_authenticated)
        return std::vector<uint8_t>(rec->peer_user_pubkey,
                                    rec->peer_user_pubkey + 32);
    return std::nullopt;
}

std::string ConnectionManager::Impl::get_peer_pubkey_hex(conn_id_t id) const {
    auto rec = rcu_find(id);
    if (rec && rec->peer_authenticated)
        return bytes_to_hex(rec->peer_user_pubkey, 32);
    return {};
}

std::optional<endpoint_t> ConnectionManager::Impl::get_peer_endpoint(conn_id_t id) const {
    auto rec = rcu_find(id);
    if (!rec) return std::nullopt;
    endpoint_t out = rec->remote;
    out.peer_id = id;
    return out;
}

// =============================================================================
// Aggregate queries
// =============================================================================

size_t ConnectionManager::Impl::connection_count() const {
    auto count = rcu_read()->size();
    LOG_TRACE("connection_count: {}", count);
    return count;
}

std::vector<std::string> ConnectionManager::Impl::get_active_uris() const {
    std::shared_lock lk(uri_mu_);
    std::vector<std::string> out; out.reserve(uri_index_.size());
    for (auto& [uri, _] : uri_index_) out.push_back(uri);
    return out;
}

std::vector<conn_id_t> ConnectionManager::Impl::get_active_conn_ids() const {
    auto map = rcu_read();
    std::vector<conn_id_t> out;
    out.reserve(map->size());
    for (auto& [id, rec] : *map)
        if (rec->state == STATE_ESTABLISHED) out.push_back(id);
    return out;
}

size_t ConnectionManager::Impl::get_pending_bytes(conn_id_t id) const noexcept {
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

// =============================================================================
// JSON diagnostic dump
// =============================================================================

std::string ConnectionManager::Impl::dump_connections() const {
    nlohmann::json arr = nlohmann::json::array();

    auto map = rcu_read();
    LOG_TRACE("dump_connections: {} records", map->size());
    for (auto& [cid, rec] : *map) {
        nlohmann::json j;
        j["id"]      = cid;
        j["state"]   = static_cast<int>(rec->state);
        j["address"] = std::string(rec->remote.address);
        j["port"]    = rec->remote.port;
        j["scheme"]  = rec->negotiated_scheme.empty()
                         ? rec->local_scheme : rec->negotiated_scheme;
        j["authenticated"] = rec->peer_authenticated;
        j["localhost"]     = rec->is_localhost;
        j["initiator"]     = rec->is_initiator;
        if (rec->peer_authenticated)
            j["peer_pubkey"] = bytes_to_hex(rec->peer_user_pubkey,
                                            GN_SIGN_PUBLICKEYBYTES);
        j["pending_bytes"] = [&]() -> size_t {
            std::shared_lock lk(queues_mu_);
            auto it = send_queues_.find(cid);
            return it != send_queues_.end()
                ? it->second->pending_bytes.load(std::memory_order_relaxed) : 0;
        }();

        // transport_paths
        nlohmann::json paths = nlohmann::json::array();
        for (const auto& tp : rec->transport_paths) {
            nlohmann::json pj;
            pj["transport_id"] = tp.transport_conn_id;
            pj["scheme"]       = tp.scheme;
            pj["priority"]     = tp.priority;
            pj["active"]       = tp.active;
            pj["rtt_us"]       = tp.last_rtt_us;
            pj["errors"]       = tp.consecutive_errors;
            paths.push_back(std::move(pj));
        }
        j["transport_paths"] = std::move(paths);

        arr.push_back(std::move(j));
    }
    return arr.dump(2);
}

} // namespace gn
