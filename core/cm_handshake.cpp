#include "connectionManager.hpp"
#include "logger.hpp"
#include <cstring>
#include <thread>
#include <chrono>

namespace gn {

std::string bytes_to_hex(const uint8_t* data, size_t len);

ConnectionManager::ConnectionManager(SignalBus& bus, NodeIdentity identity)
    : bus_(bus), identity_(std::move(identity))
{}

ConnectionManager::~ConnectionManager() = default;

// ─── fill_host_api ────────────────────────────────────────────────────────────

void ConnectionManager::fill_host_api(host_api_t* api) {
    api->ctx                = this;
    api->on_connect         = s_on_connect;
    api->on_data            = s_on_data;
    api->on_disconnect      = s_on_disconnect;
    api->send               = s_send;
    api->send_response      = s_send_response;
    api->sign_with_device   = s_sign;
    api->verify_signature   = s_verify;
    api->find_conn_by_pubkey = s_find_conn_by_pk;
    api->register_handler   = s_register_handler;
    api->log                = s_log;
    api->plugin_info        = nullptr;   // set per-plugin by PluginManager
    api->plugin_type        = PLUGIN_TYPE_UNKNOWN;  // deprecated, zero-fill
}

// ─── register_connector ───────────────────────────────────────────────────────

void ConnectionManager::register_connector(const std::string& scheme,
                                             connector_ops_t* ops) {
    std::unique_lock lock(connectors_mu_);
    connectors_[scheme] = ops;
    LOG_DEBUG("Connector '{}' registered", scheme);
}

// ─── register_handler ─────────────────────────────────────────────────────────

void ConnectionManager::register_handler(handler_t* h) {
    if (!h || !h->name) return;
    const std::string name(h->name);

    const uint8_t priority = (h->info && h->info->priority != 0)
        ? h->info->priority : 128u;

    // make_cb now satisfies HandlerPacketFn = std::function<propagation_t(...)>
    auto make_cb = [h, name](std::string_view,
                              std::shared_ptr<header_t> hdr,
                              const endpoint_t*          ep,
                              PacketData                 data) -> propagation_t
    {
        if (h->handle_message)
            h->handle_message(h->user_data, hdr.get(), ep,
                              data->data(), data->size());

        if (h->on_message_result)
            return h->on_message_result(h->user_data, hdr.get(),
                                         hdr->payload_type);

        return PROPAGATION_CONTINUE;
    };

    HandlerEntry entry; entry.name = name; entry.handler = h;

    if (h->num_supported_types == 0) {
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


// ─── set_scheme_priority / shutdown ──────────────────────────────────────────

void ConnectionManager::set_scheme_priority(std::vector<std::string> p) {
    scheme_priority_ = std::move(p);
}

void ConnectionManager::shutdown() {
    shutting_down_.store(true, std::memory_order_relaxed);

    struct ToClose { conn_id_t id; std::string scheme; };
    std::vector<ToClose> to_close;
    {
        std::shared_lock lock(records_mu_);
        for (const auto& [id, rec] : records_) {
            const auto& s = rec.negotiated_scheme.empty()
                          ? rec.local_scheme : rec.negotiated_scheme;
            to_close.push_back({id, s});
        }
    }
    for (auto& [id, scheme] : to_close)
        if (auto* ops = find_connector(scheme.empty() ? "tcp" : scheme))
            if (ops->close) ops->close(ops->connector_ctx, id);

    for (int i = 0; i < 20; ++i) {
        { std::shared_lock l(records_mu_); if (records_.empty()) break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    { std::unique_lock l(records_mu_); records_.clear(); }
    LOG_INFO("ConnectionManager: shutdown ({} connections closed)", to_close.size());
}

// ─── handle_connect ───────────────────────────────────────────────────────────

conn_id_t ConnectionManager::handle_connect(const endpoint_t* ep) {
    if (shutting_down_.load(std::memory_order_relaxed)) return CONN_ID_INVALID;

    const conn_id_t id = next_id_.fetch_add(1, std::memory_order_relaxed);

    ConnectionRecord rec;
    rec.id           = id;
    rec.remote       = *ep;
    rec.state        = STATE_AUTH_PENDING;
    rec.is_localhost = is_localhost_address(ep->address);
    rec.session      = std::make_unique<SessionState>();
    crypto_box_keypair(rec.session->my_ephem_pk, rec.session->my_ephem_sk);

    {
        std::unique_lock lk(records_mu_);
        records_[id] = std::move(rec);
    }
    {
        std::unique_lock lk(uri_mu_);
        uri_index_[std::string(ep->address) + ":" + std::to_string(ep->port)] = id;
    }

    LOG_INFO("Connect #{}: {}:{}{}", id, ep->address, ep->port,
             records_[id].is_localhost ? " [localhost]" : "");

    send_auth(id);
    return id;
}

// ─── send_auth ────────────────────────────────────────────────────────────────

void ConnectionManager::send_auth(conn_id_t id) {
    auth_payload_t ap{};

    {
        std::shared_lock id_lk(identity_mu_);
        std::memcpy(ap.user_pubkey,   identity_.user_pubkey,   32);
        std::memcpy(ap.device_pubkey, identity_.device_pubkey, 32);
    }

    {
        std::shared_lock lk(records_mu_);
        auto it = records_.find(id);
        if (it == records_.end()) return;
        std::memcpy(ap.ephem_pubkey, it->second.session->my_ephem_pk, 32);
    }

    {
        std::shared_lock id_lk(identity_mu_);
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

    send_frame(id, MSG_TYPE_AUTH, &ap, sizeof(auth_payload_t));
    LOG_DEBUG("send_auth #{}: ephem={}...", id, bytes_to_hex(ap.ephem_pubkey, 4));
}

// ─── process_auth ─────────────────────────────────────────────────────────────

bool ConnectionManager::process_auth(conn_id_t id,
                                      const uint8_t* payload, size_t size) {
    if (size < auth_payload_t::kBaseSize) {
        LOG_WARN("AUTH #{}: too small ({}/{})", id, size, auth_payload_t::kBaseSize);
        return false;
    }
    const auto* ap = reinterpret_cast<const auth_payload_t*>(payload);

    uint8_t to_verify[96];
    std::memcpy(to_verify,      ap->user_pubkey,   32);
    std::memcpy(to_verify + 32, ap->device_pubkey, 32);
    std::memcpy(to_verify + 64, ap->ephem_pubkey,  32);
    if (crypto_sign_ed25519_verify_detached(ap->signature, to_verify,
                                             sizeof(to_verify),
                                             ap->user_pubkey) != 0) {
        LOG_WARN("AUTH #{}: invalid signature", id);
        return false;
    }

    std::vector<std::string> peer_schemes;
    core_meta_t peer_meta{};

    if (size >= auth_payload_t::kBaseSize + auth_payload_t::kSchemeBlock)
        peer_schemes = ap->get_schemes();

    if (size >= auth_payload_t::kFullSize)
        peer_meta = ap->core_meta;

    {
        std::unique_lock lk(records_mu_);
        auto it = records_.find(id);
        if (it == records_.end()) return false;
        auto& rec = it->second;
        std::memcpy(rec.peer_user_pubkey,   ap->user_pubkey,   32);
        std::memcpy(rec.peer_device_pubkey, ap->device_pubkey, 32);
        rec.peer_authenticated = true;
        rec.peer_schemes       = std::move(peer_schemes);
        rec.peer_core_meta     = peer_meta;
        rec.negotiated_scheme  = negotiate_scheme(rec);
    }

    if (!derive_session(id, ap->ephem_pubkey, ap->user_pubkey)) {
        LOG_ERROR("AUTH #{}: derive_session failed", id);
        return false;
    }

    std::string peer_hex, neg_scheme;
    {
        std::unique_lock lk(records_mu_);
        auto it = records_.find(id);
        if (it == records_.end()) return false;
        it->second.state = STATE_ESTABLISHED;
        peer_hex   = bytes_to_hex(it->second.peer_user_pubkey, 4);
        neg_scheme = it->second.negotiated_scheme;
    }
    {
        std::unique_lock lk(pk_mu_);
        pk_index_[bytes_to_hex(ap->user_pubkey, 32)] = id;
    }

    LOG_INFO("AUTH #{}: peer={}... scheme='{}' core_ver=0x{:06X}{} → ESTABLISHED",
             id, peer_hex, neg_scheme, peer_meta.core_version,
             records_[id].is_localhost ? " [no-encrypt]" : "");

    {
        std::shared_lock lk(handlers_mu_);
        const std::string uri = bytes_to_hex(ap->user_pubkey, 32);
        for (auto& [name, entry] : handler_entries_)
            if (entry.handler && entry.handler->handle_conn_state)
                entry.handler->handle_conn_state(entry.handler->user_data,
                                                  uri.c_str(), STATE_ESTABLISHED);
    }
    return true;
}

// ─── handle_disconnect ────────────────────────────────────────────────────────

void ConnectionManager::handle_disconnect(conn_id_t id, int error) {
    std::string uri_key, pk_key, peer_hex;
    {
        std::unique_lock lk(records_mu_);
        auto it = records_.find(id);
        if (it == records_.end()) return;
        const auto& rec = it->second;
        uri_key = std::string(rec.remote.address) + ":"
                + std::to_string(rec.remote.port);
        if (rec.peer_authenticated) {
            pk_key   = bytes_to_hex(rec.peer_user_pubkey, 32);
            peer_hex = bytes_to_hex(rec.peer_user_pubkey, 4);
        }
        LOG_INFO("Disconnect #{} {}:{} peer={}... err={}",
                 id, rec.remote.address, rec.remote.port,
                 peer_hex.empty() ? "(unauth)" : peer_hex, error);
        records_.erase(it);
    }

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

// ─── rotate_identity_keys ────────────────────────────────────────────────────

void ConnectionManager::rotate_identity_keys(const IdentityConfig& cfg) {
    NodeIdentity next = NodeIdentity::load_or_generate(cfg);
    {
        std::unique_lock lk(identity_mu_);
        identity_ = std::move(next);
    }
    LOG_INFO("Identity rotated — user={}...",
             bytes_to_hex(identity_.user_pubkey, 4));
    // Existing sessions keep their derived session_key — no disruption (PFS).
}

// ─── local_core_meta ─────────────────────────────────────────────────────────

core_meta_t ConnectionManager::local_core_meta() {
    core_meta_t m{};
    m.core_version = GN_CORE_VERSION;
    m.caps_mask    = CORE_CAP_ZSTD | CORE_CAP_KEYROT;
    return m;
}

// ─── find_conn_by_pubkey ─────────────────────────────────────────────────────

conn_id_t ConnectionManager::find_conn_by_pubkey(const char* pubkey_hex) const {
    if (!pubkey_hex) return CONN_ID_INVALID;
    std::shared_lock lk(pk_mu_);
    auto it = pk_index_.find(pubkey_hex);
    return (it != pk_index_.end()) ? it->second : CONN_ID_INVALID;
}

// ─── Static trampolines ───────────────────────────────────────────────────────

conn_id_t ConnectionManager::s_on_connect(void* ctx, const endpoint_t* ep) {
    return static_cast<ConnectionManager*>(ctx)->handle_connect(ep);
}
void ConnectionManager::s_on_data(void* ctx, conn_id_t id,
                                   const void* raw, size_t sz) {
    static_cast<ConnectionManager*>(ctx)->handle_data(id, raw, sz);
}
void ConnectionManager::s_on_disconnect(void* ctx, conn_id_t id, int err) {
    static_cast<ConnectionManager*>(ctx)->handle_disconnect(id, err);
}
void ConnectionManager::s_send(void* ctx, const char* uri, uint32_t t,
                                const void* p, size_t sz) {
    static_cast<ConnectionManager*>(ctx)->send(uri, t, p, sz);
}
void ConnectionManager::s_send_response(void* ctx, conn_id_t cid,
                                         uint32_t t, const void* p, size_t sz) {
    static_cast<ConnectionManager*>(ctx)->send_on_conn(cid, t, p, sz);
}
int ConnectionManager::s_sign(void* ctx, const void* data, size_t sz,
                               uint8_t sig[64]) {
    const auto* self = static_cast<ConnectionManager*>(ctx);
    std::shared_lock lk(self->identity_mu_);
    return crypto_sign_ed25519_detached(
        sig, nullptr, static_cast<const uint8_t*>(data), sz,
        self->identity_.device_seckey);
}
int ConnectionManager::s_verify(void* /*ctx*/, const void* data, size_t sz,
                                 const uint8_t* pk, const uint8_t* sig) {
    return crypto_sign_ed25519_verify_detached(
        sig, static_cast<const uint8_t*>(data), sz, pk);
}
conn_id_t ConnectionManager::s_find_conn_by_pk(void* ctx, const char* hex) {
    return static_cast<ConnectionManager*>(ctx)->find_conn_by_pubkey(hex);
}
void ConnectionManager::s_register_handler(void* ctx, handler_t* h) {
    static_cast<ConnectionManager*>(ctx)->register_handler(h);
}
void ConnectionManager::s_log(void* /*ctx*/, int level,
                               const char* file, int line, const char* msg) {
    using L = spdlog::level::level_enum;
    // Map int level to spdlog level
    static constexpr L map[] = {
        L::trace, L::debug, L::info, L::warn, L::err, L::critical
    };
    const L lvl = (level >= 0 && level <= 5) ? map[level] : L::info;
    Logger::log_fmt<L::trace>(file, line, "{}", msg); // compile-time level trick
    // Use direct log_raw path via the level we actually want:
    Logger::log_fmt<L::trace>(std::string_view(file ? file : ""), line,
                               "{}", std::string_view(msg ? msg : ""));
    // Simpler — dispatch through existing macros via runtime check:
    switch (lvl) {
        case L::trace:    LOG_TRACE   ("{}", msg); break;
        case L::debug:    LOG_DEBUG   ("{}", msg); break;
        case L::info:     LOG_INFO    ("{}", msg); break;
        case L::warn:     LOG_WARN    ("{}", msg); break;
        case L::err:      LOG_ERROR   ("{}", msg); break;
        case L::critical: LOG_CRITICAL("{}", msg); break;
        default:          LOG_INFO    ("{}", msg); break;
    }
}

} // namespace gn