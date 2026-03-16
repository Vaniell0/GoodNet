/// @file core/cm_lifecycle.cpp
/// Connection lifecycle, registration.

#include "connectionManager.hpp"
#include "config.hpp"
#include "logger.hpp"

#include <cstring>
#include <thread>

#include <sodium/crypto_box.h>

#include "../sdk/connector.h"

namespace gn {

std::string bytes_to_hex(const uint8_t* data, size_t len);

// ── Construction ──────────────────────────────────────────────────────────────

ConnectionManager::ConnectionManager(SignalBus& bus, NodeIdentity identity,
                                     Config* config)
    : bus_(bus), config_(config), identity_(std::move(identity))
{
    records_rcu_.store(std::make_shared<const RecordMap>(),
                       std::memory_order_relaxed);
}

ConnectionManager::~ConnectionManager() = default;

// ── fill_host_api ─────────────────────────────────────────────────────────────

void ConnectionManager::fill_host_api(host_api_t* api) {
    api->ctx                 = this;
    api->on_connect          = s_on_connect;
    api->on_data             = s_on_data;
    api->on_disconnect       = s_on_disconnect;
    api->send                = s_send;
    api->send_response       = s_send_response;
    api->broadcast           = s_broadcast;
    api->disconnect          = s_disconnect;
    api->sign_with_device    = s_sign;
    api->verify_signature    = s_verify;
    api->find_conn_by_pubkey = s_find_conn_by_pk;
    api->get_peer_info       = s_get_peer_info;
    api->config_get          = s_config_get;
    api->register_handler    = s_register_handler;
    api->log                 = s_log;
    api->plugin_info         = nullptr;
    api->plugin_type         = PLUGIN_TYPE_UNKNOWN;
}

// ── Registration ──────────────────────────────────────────────────────────────

void ConnectionManager::register_connector(const std::string& scheme,
                                             connector_ops_t* ops) {
    std::unique_lock lock(connectors_mu_);
    connectors_[scheme] = ops;
    LOG_DEBUG("Connector '{}' registered", scheme);
}

void ConnectionManager::register_handler(handler_t* h) {
    if (!h || !h->name) return;
    const std::string name(h->name);
    const uint8_t priority = (h->info && h->info->priority) ? h->info->priority : 128u;

    auto make_cb = [h, name](std::string_view,
                              std::shared_ptr<header_t> hdr,
                              const endpoint_t*          ep,
                              PacketData                 data) -> propagation_t {
        if (h->handle_message)
            h->handle_message(h->user_data, hdr.get(), ep,
                              data->data(), data->size());
        if (h->on_message_result)
            return h->on_message_result(h->user_data, hdr.get(),
                                         hdr->payload_type);
        return PROPAGATION_CONTINUE;
    };

    HandlerEntry entry;
    entry.name    = name;
    entry.handler = h;

    if (!h->num_supported_types) {
        bus_.subscribe_wildcard(name, make_cb, priority);
        LOG_INFO("Handler '{}' registered (wildcard, prio={})", name, priority);
    } else {
        for (size_t i = 0; i < h->num_supported_types; ++i) {
            const uint32_t t = h->supported_types[i];
            bus_.subscribe(t, name, make_cb, priority);
            entry.subscribed_types.push_back(t);
        }
        LOG_INFO("Handler '{}' registered ({} types, prio={})",
                 name, h->num_supported_types, priority);
    }

    std::unique_lock lock(handlers_mu_);
    handler_entries_[name] = std::move(entry);
}

void ConnectionManager::set_scheme_priority(std::vector<std::string> p) {
    scheme_priority_ = std::move(p);
}

// ── Connection lifecycle ──────────────────────────────────────────────────────

conn_id_t ConnectionManager::handle_connect(const endpoint_t* ep) {
    if (shutting_down_.load(std::memory_order_relaxed)) return CONN_ID_INVALID;

    const conn_id_t id = next_id_.fetch_add(1, std::memory_order_relaxed);
    const bool is_local = (ep->flags & EP_FLAG_TRUSTED);

    auto rec            = std::make_shared<ConnectionRecord>();
    rec->id             = id;
    rec->remote         = *ep;
    rec->state          = STATE_AUTH_PENDING;
    rec->is_localhost   = is_local;
    rec->session        = std::make_unique<SessionState>();
    crypto_box_keypair(rec->session->my_ephem_pk, rec->session->my_ephem_sk);

    const std::string addr_key = std::string(ep->address) + ":"
                               + std::to_string(ep->port);
    {
        std::lock_guard wlk(records_write_mu_);
        rcu_update([&](RecordMap& m) { m[id] = rec; });
    }
    {
        std::unique_lock lk(uri_mu_);
        uri_index_[addr_key] = id;
    }

    // Create send queue for this connection
    {
        std::unique_lock lk(queues_mu_);
        send_queues_[id] = std::make_shared<PerConnQueue>();
    }

    LOG_DEBUG("Connect #{} {}:{}{}", id, ep->address, ep->port,
              is_local ? " [localhost]" : "");

    bus_.emit_stat({StatsEvent::Kind::Connect, 1, id});
    send_auth(id);
    return id;
}

void ConnectionManager::handle_disconnect(conn_id_t id, int error) {
    std::string uri_key, pk_key;

    {
        auto rec = rcu_find(id);
        if (!rec) return;
        uri_key = std::string(rec->remote.address) + ":"
                + std::to_string(rec->remote.port);
        if (rec->peer_authenticated)
            pk_key = bytes_to_hex(rec->peer_user_pubkey, 32);

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

    if (!uri_key.empty()) { std::unique_lock lk(uri_mu_); uri_index_.erase(uri_key); }
    if (!pk_key.empty())  { std::unique_lock lk(pk_mu_);  pk_index_.erase(pk_key);  }

    {
        std::shared_lock lk(handlers_mu_);
        for (auto& [name, entry] : handler_entries_)
            if (entry.handler && entry.handler->handle_conn_state)
                entry.handler->handle_conn_state(entry.handler->user_data,
                                                  uri_key.c_str(), STATE_CLOSED);
    }
}

void ConnectionManager::connect(std::string_view uri) {
    const std::string uri_str(uri);
    const auto sep = uri_str.find("://");
    const std::string scheme = (sep != std::string::npos) ? uri_str.substr(0, sep) : "tcp";
    if (auto* ops = find_connector(scheme))
        ops->connect(ops->connector_ctx, uri_str.c_str());
}

void ConnectionManager::disconnect(conn_id_t id) {
    auto q = [&]() -> std::shared_ptr<PerConnQueue> {
        std::shared_lock lk(queues_mu_);
        auto it = send_queues_.find(id);
        return it != send_queues_.end() ? it->second : nullptr;
    }();
    if (q) q->draining.store(true, std::memory_order_release);

    auto rec = rcu_find(id);
    if (!rec) return;
    auto* ops = find_connector(rec->negotiated_scheme.empty()
                               ? rec->local_scheme : rec->negotiated_scheme);
    if (ops && ops->close)
        ops->close(ops->connector_ctx, id);
}

void ConnectionManager::close_now(conn_id_t id) {
    auto rec = rcu_find(id);
    if (!rec) return;
    auto* ops = find_connector(rec->negotiated_scheme.empty()
                               ? rec->local_scheme : rec->negotiated_scheme);
    if (ops) {
        if (ops->close_now) ops->close_now(ops->connector_ctx, id);
        else if (ops->close) ops->close(ops->connector_ctx, id);
    }
}

void ConnectionManager::shutdown() {
    shutting_down_.store(true, std::memory_order_release);
    auto map = rcu_read();
    for (auto& [id, _] : *map) close_now(id);
}

} // namespace gn
