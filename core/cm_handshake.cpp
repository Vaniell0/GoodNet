#include "connectionManager.hpp"
#include "logger.hpp"
#include <cstring>
#include <thread>
#include <chrono>

namespace gn {

std::string bytes_to_hex(const uint8_t* data, size_t len);  // cm_identity.cpp

// ─── Constructor / Destructor ─────────────────────────────────────────────────

ConnectionManager::ConnectionManager(SignalBus& bus, NodeIdentity identity)
    : bus_(bus), identity_(std::move(identity))
{}

ConnectionManager::~ConnectionManager() = default;

// ─── fill_host_api ────────────────────────────────────────────────────────────
//
// Заполняем host_api_t коллбэками.
// Передаётся плагинам при их инициализации — единственный канал плагин↔ядро.

void ConnectionManager::fill_host_api(host_api_t* api) {
    api->ctx              = this;
    api->on_connect       = s_on_connect;
    api->on_data          = s_on_data;
    api->on_disconnect    = s_on_disconnect;
    api->send             = s_send;
    api->sign_with_device = s_sign;
    api->verify_signature = s_verify;
}

// ─── register_connector ───────────────────────────────────────────────────────

void ConnectionManager::register_connector(const std::string& scheme, connector_ops_t* ops) {
    std::unique_lock lock(connectors_mu_);
    connectors_[scheme] = ops;
    LOG_DEBUG("Connector '{}' registered", scheme);
}

// ─── register_handler ─────────────────────────────────────────────────────────
//
// Создаём канал bus[type][name] для каждого supported_type хендлера.
// Wildcard (num_supported_types == 0) → subscribe_wildcard.

void ConnectionManager::register_handler(handler_t* h) {
    if (!h || !h->name) return;
    const std::string name(h->name);

    // Замыкание: вызывает C-коллбэк хендлера handle_message()
    auto make_cb = [h, name](std::string_view,
                              std::shared_ptr<header_t> hdr,
                              const endpoint_t*          ep,
                              PacketData                 data)
    {
        if (h->handle_message)
            h->handle_message(h->user_data, hdr.get(), ep,
                              data->data(), data->size());
    };

    HandlerEntry entry; entry.name = name; entry.handler = h;

    if (h->num_supported_types == 0) {
        bus_.subscribe_wildcard(name, make_cb);
        LOG_INFO("Handler '{}' registered (wildcard)", name);
    } else {
        for (size_t i = 0; i < h->num_supported_types; ++i) {
            const uint32_t t = h->supported_types[i];
            bus_.subscribe(t, name, make_cb);
            entry.subscribed_types.push_back(t);
        }
        LOG_INFO("Handler '{}' registered ({} types)", name, h->num_supported_types);
    }

    std::unique_lock lock(handlers_mu_);
    handler_entries_[name] = std::move(entry);
}

void ConnectionManager::set_scheme_priority(std::vector<std::string> p) {
    scheme_priority_ = std::move(p);
}

// ─── shutdown ─────────────────────────────────────────────────────────────────

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
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
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

    // Предварительно генерируем эфемерный X25519 ключ
    // (затирается в cm_session.cpp после ECDH)
    rec.session = std::make_unique<SessionState>();
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
//
// Отправляем MSG_TYPE_AUTH:
//   user_pubkey || device_pubkey || sig(user_pk||device_pk||ephem_pk) || ephem_pk || schemes

void ConnectionManager::send_auth(conn_id_t id) {
    auth_payload_t ap{};
    std::memcpy(ap.user_pubkey,   identity_.user_pubkey,   32);
    std::memcpy(ap.device_pubkey, identity_.device_pubkey, 32);

    // Загружаем эфемерный pubkey из session
    {
        std::shared_lock lk(records_mu_);
        auto it = records_.find(id);
        if (it == records_.end()) return;
        std::memcpy(ap.ephem_pubkey, it->second.session->my_ephem_pk, 32);
    }

    // Подпись охватывает user_pk || device_pk || ephem_pk (60 байт)
    // Включение ephem_pk → защита от Replay Attack
    uint8_t to_sign[32 + 32 + 32];
    std::memcpy(to_sign,      identity_.user_pubkey,   32);
    std::memcpy(to_sign + 32, identity_.device_pubkey, 32);
    std::memcpy(to_sign + 64, ap.ephem_pubkey,         32);
    crypto_sign_ed25519_detached(ap.signature, nullptr,
                                  to_sign, sizeof(to_sign),
                                  identity_.user_seckey);

    ap.set_schemes(local_schemes());

    send_frame(id, MSG_TYPE_AUTH, &ap, sizeof(auth_payload_t));
    LOG_DEBUG("send_auth #{}: ephem={}...", id, bytes_to_hex(ap.ephem_pubkey, 4));
}

// ─── process_auth ─────────────────────────────────────────────────────────────
//
// Принимаем и верифицируем MSG_TYPE_AUTH от пира.
// После успешной проверки → вызываем derive_session для ECDH.

bool ConnectionManager::process_auth(conn_id_t id, const uint8_t* payload, size_t size) {
    if (size < auth_payload_t::kBaseSize) {
        LOG_WARN("AUTH #{}: too small ({}/{})", id, size, auth_payload_t::kBaseSize);
        return false;
    }
    const auto* ap = reinterpret_cast<const auth_payload_t*>(payload);

    // Верифицируем Ed25519(user_pk, user_pk||device_pk||ephem_pk)
    uint8_t to_verify[32 + 32 + 32];
    std::memcpy(to_verify,      ap->user_pubkey,   32);
    std::memcpy(to_verify + 32, ap->device_pubkey, 32);
    std::memcpy(to_verify + 64, ap->ephem_pubkey,  32);
    if (crypto_sign_ed25519_verify_detached(ap->signature, to_verify, sizeof(to_verify),
                                             ap->user_pubkey) != 0) {
        LOG_WARN("AUTH #{}: invalid signature (replay or tampered)", id);
        return false;
    }

    // Читаем схемы пира (только если пришёл расширенный формат)
    std::vector<std::string> peer_schemes;
    if (size >= auth_payload_t::kFullSize)
        peer_schemes = ap->get_schemes();

    // Обновляем ConnectionRecord
    {
        std::unique_lock lk(records_mu_);
        auto it = records_.find(id);
        if (it == records_.end()) return false;
        auto& rec = it->second;

        std::memcpy(rec.peer_user_pubkey,   ap->user_pubkey,   32);
        std::memcpy(rec.peer_device_pubkey, ap->device_pubkey, 32);
        rec.peer_authenticated = true;
        rec.peer_schemes       = std::move(peer_schemes);
        rec.negotiated_scheme  = negotiate_scheme(rec);
    }

    // ECDH → session_key
    if (!derive_session(id, ap->ephem_pubkey, ap->user_pubkey)) {
        LOG_ERROR("AUTH #{}: derive_session failed", id);
        return false;
    }

    // Переход STATE_ESTABLISHED + уведомление хендлеров
    std::string peer_hex;
    std::string neg_scheme;
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

    LOG_INFO("AUTH #{}: peer={}...  scheme='{}'{} → ESTABLISHED",
             id, peer_hex, neg_scheme,
             records_[id].is_localhost ? " [no-encrypt]" : "");

    // Уведомляем хендлеры об изменении состояния
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
    std::string uri_key, pk_key;
    std::string peer_hex;
    {
        std::unique_lock lk(records_mu_);
        auto it = records_.find(id);
        if (it == records_.end()) return;
        const auto& rec = it->second;
        uri_key  = std::string(rec.remote.address) + ":" + std::to_string(rec.remote.port);
        if (rec.peer_authenticated) {
            pk_key   = bytes_to_hex(rec.peer_user_pubkey, 32);
            peer_hex = bytes_to_hex(rec.peer_user_pubkey, 4);
        }
        LOG_INFO("Disconnect #{} {}:{} peer={}...  err={}",
                 id, rec.remote.address, rec.remote.port,
                 peer_hex.empty() ? "(unauth)" : peer_hex, error);
        records_.erase(it);
    }
    if (!uri_key.empty()) { std::unique_lock lk(uri_mu_);  uri_index_.erase(uri_key); }
    if (!pk_key.empty())  { std::unique_lock lk(pk_mu_);   pk_index_.erase(pk_key);  }

    // Уведомляем хендлеры
    {
        std::shared_lock lk(handlers_mu_);
        for (auto& [name, entry] : handler_entries_)
            if (entry.handler && entry.handler->handle_conn_state)
                entry.handler->handle_conn_state(entry.handler->user_data,
                                                  uri_key.c_str(), STATE_CLOSED);
    }
}

// ─── C-ABI адаптеры ───────────────────────────────────────────────────────────

conn_id_t ConnectionManager::s_on_connect(void* ctx, const endpoint_t* ep) {
    return static_cast<ConnectionManager*>(ctx)->handle_connect(ep);
}
void ConnectionManager::s_on_data(void* ctx, conn_id_t id, const void* raw, size_t sz) {
    static_cast<ConnectionManager*>(ctx)->handle_data(id, raw, sz);
}
void ConnectionManager::s_on_disconnect(void* ctx, conn_id_t id, int err) {
    static_cast<ConnectionManager*>(ctx)->handle_disconnect(id, err);
}
void ConnectionManager::s_send(void* ctx, const char* uri, uint32_t t,
                                const void* p, size_t sz) {
    static_cast<ConnectionManager*>(ctx)->send(uri, t, p, sz);
}
int ConnectionManager::s_sign(void* ctx, const void* data, size_t sz, uint8_t sig[64]) {
    const auto* self = static_cast<ConnectionManager*>(ctx);
    return crypto_sign_ed25519_detached(
        sig, nullptr, static_cast<const uint8_t*>(data), sz,
        self->identity_.device_seckey);
}
int ConnectionManager::s_verify(void* /*ctx*/, const void* data, size_t sz,
                                 const uint8_t* pk, const uint8_t* sig) {
    return crypto_sign_ed25519_verify_detached(
        sig, static_cast<const uint8_t*>(data), sz, pk);
}

} // namespace gn
