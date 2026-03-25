/// @file core/cm/lifecycle.cpp
/// Ctor/dtor, public API forwarding stubs, fill_host_api, connection creation,
/// connect/shutdown, local_core_meta, C-ABI trampolines.

#include "impl.hpp"
#include "config.hpp"
#include "logger.hpp"

#include <chrono>
#include <cstring>
#include <thread>

#include <sodium/crypto_sign.h>
#include <sodium/utils.h>

#include "../sdk/connector.h"

namespace gn {

// =============================================================================
// Impl construction
// =============================================================================

ConnectionManager::Impl::Impl(SignalBus& bus, NodeIdentity identity, Config* config)
    : bus_(bus), config_(config), identity_(std::move(identity))
{
    records_rcu_.store(std::make_shared<const RecordMap>(),
                       std::memory_order_relaxed);
}

// =============================================================================
// CM ctor/dtor -> Impl
// =============================================================================

ConnectionManager::ConnectionManager(SignalBus& bus, NodeIdentity identity,
                                     Config* config)
    : impl_(std::make_unique<Impl>(bus, std::move(identity), config))
{}

ConnectionManager::~ConnectionManager() = default;

// =============================================================================
// Public API forwarding stubs
// =============================================================================

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

void ConnectionManager::rotate_identity_keys(const Config::Identity& c) { impl_->rotate_identity_keys(c); }
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

// =============================================================================
// fill_host_api
// =============================================================================

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

// =============================================================================
// Connection lifecycle
// =============================================================================

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

    // Конвертируем Ed25519 device_key -> X25519 для Noise static key
    uint8_t x25519_pk[noise::DHLEN], x25519_sk[noise::DHLEN];
    {
        std::shared_lock lk(identity_mu_);
        [[maybe_unused]] int r1 = crypto_sign_ed25519_pk_to_curve25519(x25519_pk, identity_.device_pubkey);
        [[maybe_unused]] int r2 = crypto_sign_ed25519_sk_to_curve25519(x25519_sk, identity_.device_seckey);
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

void ConnectionManager::Impl::connect(std::string_view uri) {
    const std::string uri_str(uri);
    const auto sep = uri_str.find("://");
    const std::string scheme = (sep != std::string::npos) ? uri_str.substr(0, sep) : "tcp";
    LOG_TRACE("connect: uri={} scheme={}", uri_str, scheme);
    if (auto* ops = find_connector(scheme))
        ops->connect(ops->connector_ctx, uri_str.c_str());
}

// =============================================================================
// Shutdown
// =============================================================================

void ConnectionManager::Impl::shutdown() {
    LOG_DEBUG("CM shutdown initiated");
    shutting_down_.store(true, std::memory_order_release);

    // M5 fix: wait for in-flight dispatches to drain before closing connections
    LOG_TRACE("CM shutdown: waiting for {} in-flight dispatches",
              in_flight_dispatches_.load(std::memory_order_acquire));
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
    LOG_TRACE("CM shutdown: closing {} connections", map->size());
    for (auto& [id, _] : *map) close_now(id);
}

// =============================================================================
// local_core_meta
// =============================================================================

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

// =============================================================================
// C-ABI trampolines
// =============================================================================

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
