/// @file core/cm/disconnect.cpp
/// Disconnect and close logic for ConnectionManager.

#include "impl.hpp"
#include "logger.hpp"

#include "connector.h"

#include "util.hpp"

namespace gn {

// =============================================================================
// handle_disconnect
// =============================================================================

void ConnectionManager::Impl::handle_disconnect(conn_id_t id, int error) {
    LOG_TRACE("handle_disconnect #{}: err={}", id, error);
    // Проверяем: это вторичный транспорт или первичный peer?
    conn_id_t peer_id = CONN_ID_INVALID;
    {
        std::shared_lock lk(transport_mu_);
        auto it = transport_index_.find(id);
        if (it != transport_index_.end()) peer_id = it->second;
    }

    // -- Вторичный транспорт — удаляем только TransportPath -------
    if (peer_id != CONN_ID_INVALID) {
        LOG_INFO("Disconnect transport #{} (peer #{}) err={}", id, peer_id, error);

        { std::unique_lock lk(transport_mu_); transport_index_.erase(id); }

        std::string removed_scheme;
        auto rec = rcu_find(peer_id);
        if (rec) {
            // Удаляем uri_index для endpoint вторичного транспорта
            auto* tp = rec->find_path_by_transport_id(id);
            if (tp) {
                removed_scheme = tp->scheme;
                const std::string addr_key = std::string(tp->remote.address) + ":"
                                           + std::to_string(tp->remote.port);
                { std::unique_lock lk(uri_mu_); uri_index_.erase(addr_key); }
            }

            std::lock_guard wlk(records_write_mu_);
            rcu_update([&](RecordMap& m) {
                auto it = m.find(peer_id);
                if (it == m.end()) return;
                auto& paths = it->second->transport_paths;
                std::erase_if(paths, [id](const TransportPath& p) {
                    return p.transport_conn_id == id;
                });
            });
        }

        if (!removed_scheme.empty()) {
            LOG_TRACE("handle_disconnect: removed transport #{} scheme={}", id, removed_scheme);
            bus_.on_transport_change.emit(peer_id, removed_scheme, false);
        }
        return;
    }

    // -- Первичный peer — полная очистка -------
    std::string uri_key, pk_key;

    {
        auto rec = rcu_find(id);
        if (!rec) return;
        uri_key = std::string(rec->remote.address) + ":"
                + std::to_string(rec->remote.port);
        if (rec->peer_authenticated)
            pk_key = bytes_to_hex(rec->peer_user_pubkey, GN_SIGN_PUBLICKEYBYTES);

        // Удаляем transport_index_ записи для всех вторичных путей
        LOG_TRACE("handle_disconnect #{}: cleaning {} secondary paths",
                  id, rec->transport_paths.size());
        {
            std::unique_lock lk(transport_mu_);
            for (const auto& tp : rec->transport_paths) {
                if (tp.transport_conn_id != id)
                    transport_index_.erase(tp.transport_conn_id);
            }
        }

        LOG_INFO("Disconnect #{} {}:{} peer={} err={}",
                 id, rec->remote.address, rec->remote.port,
                 rec->peer_authenticated
                     ? bytes_to_hex(rec->peer_user_pubkey, 4)
                     : "(unauth)", error);
    }

    {
        std::lock_guard wlk(records_write_mu_);
        rcu_update([&](RecordMap& m) { m.erase(id); });
    }
    { std::unique_lock lk(queues_mu_); send_queues_.erase(id); }

    bus_.emit_stat({StatsEvent::Kind::Disconnect, 1, id});

    if (!uri_key.empty()) {
        { std::unique_lock lk(uri_mu_); uri_index_.erase(uri_key); }
        {
            std::unique_lock lk(pending_mu_);
            auto it = pending_messages_.find(uri_key);
            if (it != pending_messages_.end()) {
                LOG_WARN("Dropping {} pending messages for disconnected URI: {}",
                         it->second.size(), uri_key);
                pending_messages_.erase(it);
            }
        }
    }
    if (!pk_key.empty()) { std::unique_lock lk(pk_mu_); pk_index_.erase(pk_key); }

    {
        std::shared_lock lk(handlers_mu_);
        LOG_TRACE("handle_disconnect #{}: notifying {} handlers", id, handler_entries_.size());
        for (auto& [name, entry] : handler_entries_)
            if (entry.handler && entry.handler->handle_conn_state)
                entry.handler->handle_conn_state(entry.handler->user_data,
                                                  uri_key.c_str(), STATE_CLOSED);
    }
}

// =============================================================================
// disconnect (graceful — drain queue first)
// =============================================================================

void ConnectionManager::Impl::disconnect(conn_id_t id) {
    LOG_TRACE("disconnect #{}: draining queue", id);
    auto q = [&]() -> std::shared_ptr<PerConnQueue> {
        std::shared_lock lk(queues_mu_);
        auto it = send_queues_.find(id);
        return it != send_queues_.end() ? it->second : nullptr;
    }();
    if (q) q->draining.store(true, std::memory_order_release);

    auto rec = rcu_find(id);
    if (!rec) return;

    // Закрываем все транспортные пути
    bool closed_any = false;
    for (auto& tp : rec->transport_paths) {
        if (!tp.active) continue;
        auto* ops = find_connector(tp.scheme);
        if (ops && ops->close) {
            ops->close(ops->connector_ctx, tp.transport_conn_id);
            closed_any = true;
        }
    }
    // Fallback: если transport_paths пуст — старая логика
    if (!closed_any) {
        const std::string scheme = rec->negotiated_scheme.empty()
                                 ? rec->local_scheme : rec->negotiated_scheme;
        auto* ops = find_connector(scheme);
        if (ops && ops->close)
            ops->close(ops->connector_ctx, id);
    }
}

// =============================================================================
// close_now (hard close — immediate)
// =============================================================================

void ConnectionManager::Impl::close_now(conn_id_t id) {
    LOG_TRACE("close_now #{}", id);
    auto rec = rcu_find(id);
    if (!rec) return;

    // Закрываем все транспортные пути немедленно
    bool closed_any = false;
    for (auto& tp : rec->transport_paths) {
        auto* ops = find_connector(tp.scheme);
        if (!ops) continue;
        if (ops->close_now) {
            ops->close_now(ops->connector_ctx, tp.transport_conn_id);
            closed_any = true;
        } else if (ops->close) {
            ops->close(ops->connector_ctx, tp.transport_conn_id);
            closed_any = true;
        }
    }
    // Fallback: если transport_paths пуст — старая логика
    if (!closed_any) {
        const std::string scheme = rec->negotiated_scheme.empty()
                                 ? rec->local_scheme : rec->negotiated_scheme;
        auto* ops = find_connector(scheme);
        if (ops) {
            if (ops->close_now) ops->close_now(ops->connector_ctx, id);
            else if (ops->close) ops->close(ops->connector_ctx, id);
        }
    }
}

} // namespace gn
