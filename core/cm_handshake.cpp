/// @file core/cm_handshake.cpp

#include "connectionManager.hpp"
#include "config.hpp"
#include "logger.hpp"

#include <cstring>
#include <thread>
#include <sys/uio.h>

#include <sodium/crypto_box.h>
#include <sodium/crypto_sign.h>

#include "../sdk/connector.h"

namespace gn {

std::string bytes_to_hex(const uint8_t* data, size_t len);

// ── Construction ──────────────────────────────────────────────────────────────

ConnectionManager::ConnectionManager(SignalBus& bus, NodeIdentity identity)
    : bus_(bus), identity_(std::move(identity))
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
    const bool is_local = is_localhost_address(ep->address);

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
    { std::unique_lock lk(ice_mu_);    ice_states_.erase(id);  }

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

// ── Key rotation ──────────────────────────────────────────────────────────────

void ConnectionManager::rotate_identity_keys(const IdentityConfig& cfg) {
    NodeIdentity next = NodeIdentity::load_or_generate(cfg);
    std::unique_lock lk(identity_mu_);
    identity_ = std::move(next);
    LOG_INFO("Identity rotated — user={}...", bytes_to_hex(identity_.user_pubkey, 4));
}

bool ConnectionManager::rekey_session(conn_id_t id) {
    auto rec = rcu_find(id);
    if (!rec || rec->state != STATE_ESTABLISHED) return false;

    // Generate new ephemeral keypair for this connection
    uint8_t new_ephem_pk[32]{}, new_ephem_sk[32]{};
    crypto_box_keypair(new_ephem_pk, new_ephem_sk);

    // Store it temporarily in the session (session remains valid until new one derived)
    {
        std::lock_guard wlk(records_write_mu_);
        rcu_update([&](RecordMap& m) {
            auto it = m.find(id);
            if (it == m.end()) return;
            auto& s = *it->second->session;
            std::memcpy(s.my_ephem_pk, new_ephem_pk, 32);
            std::memcpy(s.my_ephem_sk, new_ephem_sk, 32);
            // Reset nonces for the new session
            s.send_nonce.store(1, std::memory_order_relaxed);
            s.recv_nonce_expected.store(1, std::memory_order_relaxed);
        });
    }

    // Build and send KEY_EXCHANGE payload: [ephem_pk(32) | sig(64)]
    uint8_t keyex[96]{};
    std::memcpy(keyex, new_ephem_pk, 32);
    {
        std::shared_lock lk(identity_mu_);
        crypto_sign_ed25519_detached(keyex + 32, nullptr,
                                      new_ephem_pk, 32,
                                      identity_.device_seckey);
    }

    send_frame(id, MSG_TYPE_KEY_EXCHANGE,
               std::span<const uint8_t>(keyex, sizeof(keyex)));
    LOG_INFO("rekey_session #{}: sent KEY_EXCHANGE ephem={}...",
             id, bytes_to_hex(new_ephem_pk, 4));
    return true;
}

// ── ICE / P2P ─────────────────────────────────────────────────────────────────

void ConnectionManager::initiate_ice(conn_id_t id) {
    auto rec = rcu_find(id);
    if (!rec || rec->state != STATE_ESTABLISHED) return;
    {
        std::unique_lock lk(ice_mu_);
        ice_states_[id] = IceState::Gathering;
    }
    // Plugins (libnice) handle actual SDP generation via MSG_TYPE_ICE_SIGNAL.
    // The core just maintains state and routes the message.
    // Signal the ice plugin via bus to start gathering:
    auto dummy_hdr  = std::make_shared<header_t>();
    dummy_hdr->payload_type = MSG_TYPE_ICE_SIGNAL;
    auto empty_data = std::make_shared<sdk::RawBuffer>();
    endpoint_t ep   = rec->remote;
    ep.peer_id      = id;
    bus_.dispatch_packet(MSG_TYPE_ICE_SIGNAL, dummy_hdr, &ep, empty_data);
    LOG_INFO("initiate_ice #{}: ICE gathering started", id);
}

void ConnectionManager::handle_ice_signal(conn_id_t id,
                                           std::span<const uint8_t> payload) {
    {
        std::unique_lock lk(ice_mu_);
        auto& st = ice_states_[id];
        if (st == IceState::Idle) st = IceState::Connecting;
    }
    // Forward to ICE plugin via normal dispatch (MSG_TYPE_ICE_SIGNAL).
    auto rec = rcu_find(id);
    if (!rec) return;
    auto hdr_ptr  = std::make_shared<header_t>();
    hdr_ptr->payload_type = MSG_TYPE_ICE_SIGNAL;
    hdr_ptr->payload_len  = static_cast<uint32_t>(payload.size());
    auto data_ptr = std::make_shared<sdk::RawBuffer>(payload.begin(), payload.end());
    endpoint_t ep = rec->remote;
    ep.peer_id    = id;
    bus_.dispatch_packet(MSG_TYPE_ICE_SIGNAL, hdr_ptr, &ep, data_ptr);
}

IceState ConnectionManager::ice_state(conn_id_t id) const {
    std::shared_lock lk(ice_mu_);
    auto it = ice_states_.find(id);
    return it != ice_states_.end() ? it->second : IceState::Idle;
}

// ── AUTH ──────────────────────────────────────────────────────────────────────

void ConnectionManager::send_auth(conn_id_t id) {
    msg::AuthPayload ap{};
    {
        std::shared_lock lk(identity_mu_);
        std::memcpy(ap.user_pubkey,   identity_.user_pubkey,   32);
        std::memcpy(ap.device_pubkey, identity_.device_pubkey, 32);
    }
    {
        auto rec = rcu_find(id);
        if (!rec) return;
        std::memcpy(ap.ephem_pubkey, rec->session->my_ephem_pk, 32);
    }
    {
        std::shared_lock lk(identity_mu_);
        uint8_t to_sign[96];
        std::memcpy(to_sign,      ap.user_pubkey,   32);
        std::memcpy(to_sign + 32, ap.device_pubkey, 32);
        std::memcpy(to_sign + 64, ap.ephem_pubkey,  32);
        crypto_sign_ed25519_detached(ap.signature, nullptr,
                                      to_sign, sizeof(to_sign),
                                      identity_.user_seckey);
    }
    ap.set_schemes(local_schemes());
    ap.core_meta = local_core_meta();

    send_frame(id, MSG_TYPE_AUTH,
               std::span<const uint8_t>(
                   reinterpret_cast<const uint8_t*>(&ap), sizeof(ap)));
    LOG_DEBUG("send_auth #{}: ephem={}...", id, bytes_to_hex(ap.ephem_pubkey, 4));
}

bool ConnectionManager::process_auth(conn_id_t id, std::span<const uint8_t> sp) {
    if (sp.size() < msg::AuthPayload::kBaseSize) {
        bus_.emit_drop(id, DropReason::AuthFail);
        return false;
    }
    const auto* ap = reinterpret_cast<const msg::AuthPayload*>(sp.data());

    uint8_t to_verify[96];
    std::memcpy(to_verify,      ap->user_pubkey,   32);
    std::memcpy(to_verify + 32, ap->device_pubkey, 32);
    std::memcpy(to_verify + 64, ap->ephem_pubkey,  32);
    if (crypto_sign_ed25519_verify_detached(ap->signature, to_verify,
                                             sizeof(to_verify),
                                             ap->user_pubkey) != 0) {
        LOG_WARN("AUTH #{}: invalid signature", id);
        bus_.emit_drop(id, DropReason::AuthFail);
        return false;
    }

    std::vector<std::string> peer_schemes;
    msg::CoreMeta peer_meta{};
    if (sp.size() >= msg::AuthPayload::kBaseSize + msg::AuthPayload::kSchemeBlock)
        peer_schemes = ap->get_schemes();
    if (sp.size() >= msg::AuthPayload::kFullSize)
        peer_meta = ap->core_meta;

    {
        std::lock_guard wlk(records_write_mu_);
        rcu_update([&](RecordMap& m) {
            auto it = m.find(id);
            if (it == m.end()) return;
            auto& rec = *it->second;
            std::memcpy(rec.peer_user_pubkey,   ap->user_pubkey,   32);
            std::memcpy(rec.peer_device_pubkey, ap->device_pubkey, 32);
            rec.peer_authenticated = true;
            rec.peer_schemes       = std::move(peer_schemes);
            rec.peer_core_meta     = peer_meta;
            rec.negotiated_scheme  = negotiate_scheme(rec);
        });
    }

    if (!derive_session(id, ap->ephem_pubkey, ap->user_pubkey)) {
        bus_.emit_drop(id, DropReason::AuthFail);
        return false;
    }

    {
        std::lock_guard wlk(records_write_mu_);
        rcu_update([&](RecordMap& m) {
            if (auto it = m.find(id); it != m.end())
                it->second->state = STATE_ESTABLISHED;
        });
        std::unique_lock lk(pk_mu_);
        pk_index_[bytes_to_hex(ap->user_pubkey, 32)] = id;
    }

    auto rec = rcu_find(id);
    LOG_INFO("AUTH #{}: peer={}... scheme='{}' → ESTABLISHED",
             id, bytes_to_hex(ap->user_pubkey, 4),
             rec ? rec->negotiated_scheme : "?");

    {
        const std::string uri = bytes_to_hex(ap->user_pubkey, 32);
        std::shared_lock lk(handlers_mu_);
        for (auto& [name, entry] : handler_entries_)
            if (entry.handler && entry.handler->handle_conn_state)
                entry.handler->handle_conn_state(entry.handler->user_data,
                                                  uri.c_str(), STATE_ESTABLISHED);
    }
    bus_.on_conn_state.emit(id, STATE_ESTABLISHED);
    return true;
}

bool ConnectionManager::process_keyex(conn_id_t id, std::span<const uint8_t> sp) {
    if (sp.size() < 96) return false;
    const uint8_t* peer_ephem_pk = sp.data();
    const uint8_t* sig           = sp.data() + 32;

    // Verify signature over the new ephem_pk using peer's already-known device key
    auto rec = rcu_find(id);
    if (!rec || !rec->peer_authenticated) return false;

    if (crypto_sign_ed25519_verify_detached(sig, peer_ephem_pk, 32,
                                             rec->peer_device_pubkey) != 0) {
        LOG_WARN("KEY_EXCHANGE #{}: bad signature", id);
        return false;
    }

    if (!derive_session(id, peer_ephem_pk, rec->peer_user_pubkey)) return false;
    LOG_INFO("KEY_EXCHANGE #{}: session rekeyed ephem={}...",
             id, bytes_to_hex(peer_ephem_pk, 4));
    return true;
}

// ── local_core_meta / find_conn_by_pubkey ────────────────────────────────────

msg::CoreMeta ConnectionManager::local_core_meta() {
    msg::CoreMeta m{};
    m.core_version = GN_CORE_VERSION;
    m.caps_mask    = CORE_CAP_ZSTD | CORE_CAP_KEYROT;
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

// ── Static trampolines ────────────────────────────────────────────────────────

conn_id_t ConnectionManager::s_on_connect(void* ctx, const endpoint_t* ep) {
    return static_cast<ConnectionManager*>(ctx)->handle_connect(ep); }
void ConnectionManager::s_on_data(void* ctx, conn_id_t id, const void* r, size_t sz) {
    static_cast<ConnectionManager*>(ctx)->handle_data(id, r, sz); }
void ConnectionManager::s_on_disconnect(void* ctx, conn_id_t id, int err) {
    static_cast<ConnectionManager*>(ctx)->handle_disconnect(id, err); }
void ConnectionManager::s_send(void* ctx, const char* uri, uint32_t t,
                                const void* p, size_t sz) {
    static_cast<ConnectionManager*>(ctx)->send(uri, t, p, sz); }
void ConnectionManager::s_send_response(void* ctx, conn_id_t id, uint32_t t,
                                         const void* p, size_t sz) {
    static_cast<ConnectionManager*>(ctx)->send_on_conn(id, t, p, sz); }
void ConnectionManager::s_broadcast(void* ctx, uint32_t t, const void* p, size_t sz) {
    static_cast<ConnectionManager*>(ctx)->broadcast(t, p, sz); }
void ConnectionManager::s_disconnect(void* ctx, conn_id_t id) {
    static_cast<ConnectionManager*>(ctx)->disconnect(id); }
int ConnectionManager::s_sign(void* ctx, const void* data, size_t sz, uint8_t sig[64]) {
    const auto* self = static_cast<ConnectionManager*>(ctx);
    std::shared_lock lk(self->identity_mu_);
    return crypto_sign_ed25519_detached(sig, nullptr,
        static_cast<const uint8_t*>(data), sz, self->identity_.device_seckey); }
int ConnectionManager::s_verify(void*, const void* data, size_t sz,
                                 const uint8_t* pk, const uint8_t* sig) {
    return crypto_sign_ed25519_verify_detached(
        sig, static_cast<const uint8_t*>(data), sz, pk); }
conn_id_t ConnectionManager::s_find_conn_by_pk(void* ctx, const char* hex) {
    return static_cast<ConnectionManager*>(ctx)->find_conn_by_pubkey(hex); }
int ConnectionManager::s_get_peer_info(void* ctx, conn_id_t id, endpoint_t* ep) {
    return static_cast<ConnectionManager*>(ctx)->get_peer_endpoint(id, *ep) ? 0 : -1; }
int ConnectionManager::s_config_get(void* /*ctx*/, const char* key,
                                     char* buf, size_t sz) {
    // Config is not injected into CM — forward via a thread-local pointer set by Core
    // For now, return -1; Core::fill_host_api overrides this with a lambda closure.
    (void)key; (void)buf; (void)sz;
    return -1;
}
void ConnectionManager::s_register_handler(void* ctx, handler_t* h) {
    static_cast<ConnectionManager*>(ctx)->register_handler(h); }
void ConnectionManager::s_log(void*, int level, const char* file, int line, const char* msg) {
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