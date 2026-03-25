/// @file core/cm_lifecycle.cpp
/// Ctor/dtor, public API stubs, registration, connection lifecycle, queries, CAPI trampolines.

#include "cm_impl.hpp"
#include "config.hpp"
#include "logger.hpp"

#include <chrono>
#include <cstring>
#include <thread>

#include <nlohmann/json.hpp>
#include <sodium/crypto_sign.h>
#include <sodium/utils.h>

#include "../sdk/connector.h"

#include "util.hpp"

namespace gn {

// ═══════════════════════════════════════════════════════════════════════════════
// Impl construction
// ═══════════════════════════════════════════════════════════════════════════════

ConnectionManager::Impl::Impl(SignalBus& bus, NodeIdentity identity, Config* config)
    : bus_(bus), config_(config), identity_(std::move(identity))
{
    records_rcu_.store(std::make_shared<const RecordMap>(),
                       std::memory_order_relaxed);
}

// ═══════════════════════════════════════════════════════════════════════════════
// CM ctor/dtor → Impl
// ═══════════════════════════════════════════════════════════════════════════════

ConnectionManager::ConnectionManager(SignalBus& bus, NodeIdentity identity,
                                     Config* config)
    : impl_(std::make_unique<Impl>(bus, std::move(identity), config))
{}

ConnectionManager::~ConnectionManager() = default;

// ═══════════════════════════════════════════════════════════════════════════════
// Public API forwarding stubs
// ═══════════════════════════════════════════════════════════════════════════════

void ConnectionManager::register_handler(handler_t* h)                                    { impl_->register_handler(h); }
void ConnectionManager::register_connector(const std::string& s, connector_ops_t* o)      { impl_->register_connector(s, o); }
void ConnectionManager::set_scheme_priority(std::vector<std::string> p)                   { impl_->set_scheme_priority(std::move(p)); }
void ConnectionManager::fill_host_api(host_api_t* api)                                    { impl_->fill_host_api(api); }

bool ConnectionManager::send(std::string_view u, uint32_t t, std::span<const uint8_t> p)  { return impl_->send(u, t, p); }
bool ConnectionManager::send(conn_id_t id, uint32_t t, std::span<const uint8_t> p)        { return impl_->send(id, t, p); }
void ConnectionManager::broadcast(uint32_t t, std::span<const uint8_t> p)                 { impl_->broadcast(t, p); }

void ConnectionManager::connect(std::string_view uri) { impl_->connect(uri); }
void ConnectionManager::disconnect(conn_id_t id)      { impl_->disconnect(id); }
void ConnectionManager::close_now(conn_id_t id)       { impl_->close_now(id); }
void ConnectionManager::shutdown()                     { impl_->shutdown(); }

void ConnectionManager::rotate_identity_keys(const IdentityConfig& c) { impl_->rotate_identity_keys(c); }
bool ConnectionManager::rekey_session(conn_id_t id)                   { return impl_->rekey_session(id); }

void ConnectionManager::relay(conn_id_t e, uint8_t t,
                               const uint8_t pk[GN_SIGN_PUBLICKEYBYTES],
                               std::span<const uint8_t> f) { impl_->relay(e, t, pk, f); }

void ConnectionManager::check_heartbeat_timeouts() { impl_->check_heartbeat_timeouts(); }
void ConnectionManager::cleanup_stale_pending()    { impl_->cleanup_stale_pending(); }

size_t                      ConnectionManager::connection_count()                    const { return impl_->connection_count(); }
std::vector<std::string>    ConnectionManager::get_active_uris()                     const { return impl_->get_active_uris(); }
std::vector<conn_id_t>      ConnectionManager::get_active_conn_ids()                 const { return impl_->get_active_conn_ids(); }
std::optional<conn_state_t> ConnectionManager::get_state(conn_id_t id)               const { return impl_->get_state(id); }
std::optional<std::string>  ConnectionManager::get_negotiated_scheme(conn_id_t id)   const { return impl_->get_negotiated_scheme(id); }
std::optional<std::vector<uint8_t>> ConnectionManager::get_peer_pubkey(conn_id_t id) const { return impl_->get_peer_pubkey(id); }
std::string                 ConnectionManager::get_peer_pubkey_hex(conn_id_t id)     const { return impl_->get_peer_pubkey_hex(id); }
std::optional<endpoint_t>   ConnectionManager::get_peer_endpoint(conn_id_t id)       const { return impl_->get_peer_endpoint(id); }
conn_id_t                   ConnectionManager::find_conn_by_pubkey(const char* h)    const { return impl_->find_conn_by_pubkey(h); }
size_t ConnectionManager::get_pending_bytes(conn_id_t id) const noexcept { return impl_->get_pending_bytes(id); }
std::string ConnectionManager::dump_connections() const { return impl_->dump_connections(); }

const NodeIdentity& ConnectionManager::identity() const        { return impl_->identity_; }
msg::CoreMeta       ConnectionManager::local_core_meta() const { return impl_->local_core_meta(); }

// ═══════════════════════════════════════════════════════════════════════════════
// fill_host_api
// ═══════════════════════════════════════════════════════════════════════════════

void ConnectionManager::Impl::fill_host_api(host_api_t* api) {
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
    api->add_transport       = s_add_transport;
    api->log                 = s_log;
    api->plugin_info         = nullptr;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Registration
// ═══════════════════════════════════════════════════════════════════════════════

void ConnectionManager::Impl::register_connector(const std::string& scheme,
                                                   connector_ops_t* ops) {
    std::unique_lock lock(connectors_mu_);
    connectors_[scheme] = ops;
    LOG_DEBUG("Connector '{}' registered", scheme);
}

void ConnectionManager::Impl::register_handler(handler_t* h) {
    register_handler_internal(h, HandlerSource::Plugin);
}

void ConnectionManager::Impl::register_handler_from_connector(handler_t* h) {
    register_handler_internal(h, HandlerSource::Connector);
}

bool ConnectionManager::Impl::is_connector_blocked_type(uint32_t msg_type) {
    // Типы 0-9: ядро (SYSTEM, NOISE_INIT/RESP/FIN, HEARTBEAT, резерв)
    if (msg_type <= 9) return true;
    // Системный диапазон 0x0100-0x0FFF: DHT, Health, RPC, Routing, TUN
    if (msg_type >= MSG_TYPE_SYS_BASE && msg_type <= MSG_TYPE_SYS_MAX) return true;
    return false;
}

void ConnectionManager::Impl::register_handler_internal(handler_t* h,
                                                          HandlerSource source) {
    if (!h || !h->name) return;
    const std::string name(h->name);
    const uint8_t priority = (h->info && h->info->priority) ? h->info->priority : 128u;

    // Коннекторам запрещена wildcard-подписка
    if (source == HandlerSource::Connector && !h->num_supported_types) {
        LOG_WARN("register_handler '{}': wildcard subscription blocked "
                 "for connector-registered handler", name);
        return;
    }

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
    entry.source  = source;

    if (!h->num_supported_types) {
        bus_.subscribe_wildcard(name, make_cb, priority);
        LOG_INFO("Handler '{}' registered (wildcard, prio={})", name, priority);
    } else {
        for (size_t i = 0; i < h->num_supported_types; ++i) {
            const uint32_t t = h->supported_types[i];

            // Валидация для connector-registered handlers
            if (source == HandlerSource::Connector && is_connector_blocked_type(t)) {
                LOG_WARN("register_handler '{}': type {} blocked for "
                         "connector handler — skipped", name, t);
                continue;
            }

            bus_.subscribe(t, name, make_cb, priority);
            entry.subscribed_types.push_back(t);
        }

        if (entry.subscribed_types.empty()) {
            LOG_WARN("register_handler '{}': all types blocked — handler not registered",
                     name);
            return;
        }

        LOG_INFO("Handler '{}' registered ({} types, prio={}, source={})",
                 name, entry.subscribed_types.size(), priority,
                 source == HandlerSource::Connector ? "connector" : "plugin");
    }

    std::unique_lock lock(handlers_mu_);
    handler_entries_[name] = std::move(entry);
}

void ConnectionManager::Impl::set_scheme_priority(std::vector<std::string> p) {
    scheme_priority_ = std::move(p);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Connection lifecycle
// ═══════════════════════════════════════════════════════════════════════════════

conn_id_t ConnectionManager::Impl::handle_connect(const endpoint_t* ep) {
    LOG_TRACE("handle_connect: {}:{} flags=0x{:02X}", ep->address, ep->port, ep->flags);
    if (shutting_down_.load(std::memory_order_relaxed)) return CONN_ID_INVALID;

    const conn_id_t id = next_id_.fetch_add(1, std::memory_order_relaxed);
    const bool is_local = (ep->flags & EP_FLAG_TRUSTED);
    const bool is_outbound = (ep->flags & EP_FLAG_OUTBOUND);

    auto rec            = std::make_shared<ConnectionRecord>();
    rec->id             = id;
    rec->remote         = *ep;
    rec->state          = STATE_NOISE_HANDSHAKE;
    rec->is_localhost   = is_local;
    rec->is_initiator   = is_outbound;

    // Конвертируем Ed25519 device_key → X25519 для Noise static key
    uint8_t x25519_pk[noise::DHLEN], x25519_sk[noise::DHLEN];
    {
        std::shared_lock lk(identity_mu_);
        crypto_sign_ed25519_pk_to_curve25519(x25519_pk, identity_.device_pubkey);
        crypto_sign_ed25519_sk_to_curve25519(x25519_sk, identity_.device_seckey);
    }

    rec->handshake = std::make_unique<noise::HandshakeState>();
    rec->handshake->init(is_outbound, x25519_pk, x25519_sk);
    sodium_memzero(x25519_sk, sizeof(x25519_sk));

    // Начальный транспортный путь (scheme обновится после handshake negotiate)
    {
        TransportPath tp;
        tp.transport_conn_id = id;
        tp.scheme            = "tcp"; // default, обновится в negotiate_scheme
        tp.priority          = scheme_priority_index("tcp");
        tp.remote            = *ep;
        tp.added_at          = std::chrono::steady_clock::now();
        rec->transport_paths.push_back(std::move(tp));
    }

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
    {
        std::unique_lock lk(queues_mu_);
        send_queues_[id] = std::make_shared<PerConnQueue>();
    }

    LOG_DEBUG("Connect #{} {}:{}{}{}", id, ep->address, ep->port,
              is_local ? " [localhost]" : "",
              is_outbound ? " [outbound]" : "");

    bus_.emit_stat({StatsEvent::Kind::Connect, 1, id});

    // Initiator отправляет первое Noise сообщение
    if (is_outbound) {
        send_noise_init(id);
    }
    return id;
}

conn_id_t ConnectionManager::Impl::handle_add_transport(const char* pubkey_hex,
                                                          const endpoint_t* ep,
                                                          const char* scheme) {
    if (!pubkey_hex || !ep || !scheme) return CONN_ID_INVALID;
    if (shutting_down_.load(std::memory_order_relaxed)) return CONN_ID_INVALID;

    // Ищем существующий ESTABLISHED peer по pubkey
    const conn_id_t peer_id = find_conn_by_pubkey(pubkey_hex);
    if (peer_id == CONN_ID_INVALID) {
        LOG_WARN("add_transport: peer '{}...' not found", std::string(pubkey_hex, 8));
        return CONN_ID_INVALID;
    }

    auto rec = rcu_find(peer_id);
    if (!rec || rec->state != STATE_ESTABLISHED) {
        LOG_WARN("add_transport: peer #{} not ESTABLISHED", peer_id);
        return CONN_ID_INVALID;
    }

    // Проверяем что такой scheme ещё не добавлен
    const std::string scheme_str(scheme);
    if (rec->find_path(scheme_str)) {
        LOG_WARN("add_transport: peer #{} already has '{}' path", peer_id, scheme_str);
        return CONN_ID_INVALID;
    }

    // Выделяем transport_conn_id для нового пути
    const conn_id_t transport_id = next_id_.fetch_add(1, std::memory_order_relaxed);

    TransportPath tp;
    tp.transport_conn_id = transport_id;
    tp.scheme            = scheme_str;
    tp.priority          = scheme_priority_index(scheme_str);
    tp.remote            = *ep;
    tp.added_at          = std::chrono::steady_clock::now();

    {
        std::lock_guard wlk(records_write_mu_);
        rcu_update([&](RecordMap& m) {
            auto it = m.find(peer_id);
            if (it == m.end()) return;
            it->second->transport_paths.push_back(std::move(tp));
        });
    }
    {
        std::unique_lock lk(transport_mu_);
        transport_index_[transport_id] = peer_id;
    }

    const std::string addr_key = std::string(ep->address) + ":"
                               + std::to_string(ep->port);
    {
        std::unique_lock lk(uri_mu_);
        uri_index_[addr_key] = peer_id;
    }

    LOG_INFO("add_transport: peer #{} +{} (transport_id={}) {}:{}",
             peer_id, scheme_str, transport_id, ep->address, ep->port);
    bus_.on_transport_change.emit(peer_id, scheme_str, true);
    return transport_id;
}

void ConnectionManager::Impl::handle_disconnect(conn_id_t id, int error) {
    // Проверяем: это вторичный транспорт или первичный peer?
    conn_id_t peer_id = CONN_ID_INVALID;
    {
        std::shared_lock lk(transport_mu_);
        auto it = transport_index_.find(id);
        if (it != transport_index_.end()) peer_id = it->second;
    }

    // ── Вторичный транспорт — удаляем только TransportPath ──────────────────
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

        if (!removed_scheme.empty())
            bus_.on_transport_change.emit(peer_id, removed_scheme, false);
        return;
    }

    // ── Первичный peer — полная очистка ─────────────────────────────────────
    std::string uri_key, pk_key;

    {
        auto rec = rcu_find(id);
        if (!rec) return;
        uri_key = std::string(rec->remote.address) + ":"
                + std::to_string(rec->remote.port);
        if (rec->peer_authenticated)
            pk_key = bytes_to_hex(rec->peer_user_pubkey, crypto_sign_PUBLICKEYBYTES);

        // Удаляем transport_index_ записи для всех вторичных путей
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
        for (auto& [name, entry] : handler_entries_)
            if (entry.handler && entry.handler->handle_conn_state)
                entry.handler->handle_conn_state(entry.handler->user_data,
                                                  uri_key.c_str(), STATE_CLOSED);
    }
}

void ConnectionManager::Impl::connect(std::string_view uri) {
    const std::string uri_str(uri);
    const auto sep = uri_str.find("://");
    const std::string scheme = (sep != std::string::npos) ? uri_str.substr(0, sep) : "tcp";
    if (auto* ops = find_connector(scheme))
        ops->connect(ops->connector_ctx, uri_str.c_str());
}

void ConnectionManager::Impl::disconnect(conn_id_t id) {
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

void ConnectionManager::Impl::close_now(conn_id_t id) {
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

void ConnectionManager::Impl::shutdown() {
    LOG_DEBUG("CM shutdown initiated");
    shutting_down_.store(true, std::memory_order_release);

    // M5 fix: wait for in-flight dispatches to drain before closing connections
    for (int spins = 0;
         in_flight_dispatches_.load(std::memory_order_acquire) > 0;
         ++spins)
    {
        if (spins < 128)
            std::this_thread::yield();
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    auto map = rcu_read();
    for (auto& [id, _] : *map) close_now(id);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Queries
// ═══════════════════════════════════════════════════════════════════════════════

msg::CoreMeta ConnectionManager::Impl::local_core_meta() const {
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

conn_id_t ConnectionManager::Impl::find_conn_by_pubkey(const char* pubkey_hex) const {
    if (!pubkey_hex) return CONN_ID_INVALID;
    std::shared_lock lk(pk_mu_);
    auto it = pk_index_.find(pubkey_hex);
    return it != pk_index_.end() ? it->second : CONN_ID_INVALID;
}

size_t ConnectionManager::Impl::connection_count() const {
    return rcu_read()->size();
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

std::string ConnectionManager::Impl::dump_connections() const {
    nlohmann::json arr = nlohmann::json::array();

    auto map = rcu_read();
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
                                            crypto_sign_PUBLICKEYBYTES);
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

// ═══════════════════════════════════════════════════════════════════════════════
// C-ABI trampolines
// ═══════════════════════════════════════════════════════════════════════════════

// Verify SDK constants match libsodium
static_assert(GN_SIGN_PUBLICKEYBYTES == crypto_sign_PUBLICKEYBYTES);
static_assert(GN_SIGN_SECRETKEYBYTES == crypto_sign_SECRETKEYBYTES);
static_assert(GN_SIGN_BYTES          == crypto_sign_BYTES);

conn_id_t ConnectionManager::Impl::s_on_connect(void* ctx, const endpoint_t* ep) {
    return static_cast<Impl*>(ctx)->handle_connect(ep); }
void ConnectionManager::Impl::s_on_data(void* ctx, conn_id_t id, const void* r, size_t sz) {
    static_cast<Impl*>(ctx)->handle_data(id, r, sz); }
void ConnectionManager::Impl::s_on_disconnect(void* ctx, conn_id_t id, int err) {
    static_cast<Impl*>(ctx)->handle_disconnect(id, err); }
void ConnectionManager::Impl::s_send(void* ctx, const char* uri, uint32_t t,
                                      const void* p, size_t sz) {
    static_cast<Impl*>(ctx)->send(
        std::string_view(uri), t, std::span{static_cast<const uint8_t*>(p), sz}); }
void ConnectionManager::Impl::s_send_response(void* ctx, conn_id_t id, uint32_t t,
                                               const void* p, size_t sz) {
    static_cast<Impl*>(ctx)->send(
        id, t, std::span{static_cast<const uint8_t*>(p), sz}); }
void ConnectionManager::Impl::s_broadcast(void* ctx, uint32_t t, const void* p, size_t sz) {
    static_cast<Impl*>(ctx)->broadcast(
        t, std::span{static_cast<const uint8_t*>(p), sz}); }
void ConnectionManager::Impl::s_disconnect(void* ctx, conn_id_t id) {
    static_cast<Impl*>(ctx)->disconnect(id); }
int ConnectionManager::Impl::s_sign(void* ctx, const void* data, size_t sz,
                                     uint8_t sig[GN_SIGN_BYTES]) {
    const auto* self = static_cast<Impl*>(ctx);
    std::shared_lock lk(self->identity_mu_);
    return crypto_sign_ed25519_detached(sig, nullptr,
        static_cast<const uint8_t*>(data), sz, self->identity_.device_seckey); }
int ConnectionManager::Impl::s_verify(void*, const void* data, size_t sz,
                                       const uint8_t* pk, const uint8_t* sig) {
    return crypto_sign_ed25519_verify_detached(
        sig, static_cast<const uint8_t*>(data), sz, pk); }
conn_id_t ConnectionManager::Impl::s_find_conn_by_pk(void* ctx, const char* hex) {
    return static_cast<Impl*>(ctx)->find_conn_by_pubkey(hex); }
int ConnectionManager::Impl::s_get_peer_info(void* ctx, conn_id_t id, endpoint_t* ep) {
    auto opt = static_cast<Impl*>(ctx)->get_peer_endpoint(id);
    if (!opt) return -1;
    *ep = *opt;
    return 0; }
int ConnectionManager::Impl::s_config_get(void* ctx, const char* key,
                                            char* buf, size_t sz) {
    auto* self = static_cast<Impl*>(ctx);
    if (!self->config_) return -1;
    auto v = self->config_->get_raw(key);
    if (!v) return -1;
    std::strncpy(buf, v->c_str(), sz - 1);
    buf[sz - 1] = '\0';
    return static_cast<int>(v->size());
}
void ConnectionManager::Impl::s_register_handler(void* ctx, handler_t* h) {
    static_cast<Impl*>(ctx)->register_handler_from_connector(h); }
conn_id_t ConnectionManager::Impl::s_add_transport(void* ctx, const char* pk,
                                                     const endpoint_t* ep, const char* s) {
    return static_cast<Impl*>(ctx)->handle_add_transport(pk, ep, s); }
void ConnectionManager::Impl::s_log(void*, int level, const char* file, int line,
                                     const char* msg) {
    (void)file; (void)line;
    switch (level) {
        case 0: LOG_TRACE   ("{}", msg); break;
        case 1: LOG_DEBUG   ("{}", msg); break;
        case 2: LOG_INFO    ("{}", msg); break;
        case 3: LOG_WARN    ("{}", msg); break;
        case 4: LOG_ERROR   ("{}", msg); break;
        default:LOG_CRITICAL("{}", msg); break;
    }
}

} // namespace gn
