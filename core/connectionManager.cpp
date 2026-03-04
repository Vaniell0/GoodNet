#include "connectionManager.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace fs = std::filesystem;
namespace gn {

// ─── Утилиты ─────────────────────────────────────────────────────────────────

static std::string bytes_to_hex(const uint8_t* d, size_t n) {
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i)
        fmt::format_to(std::back_inserter(out), "{:02x}", d[i]);
    return out;
}

static bool save_key(const fs::path& p, const uint8_t* k, size_t n) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(k), static_cast<std::streamsize>(n));
    f.close();
    fs::permissions(p, fs::perms::owner_read | fs::perms::owner_write);
    return true;
}

static bool load_key(const fs::path& p, uint8_t* k, size_t n) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    f.read(reinterpret_cast<char*>(k), static_cast<std::streamsize>(n));
    return static_cast<size_t>(f.gcount()) == n;
}

// ─── NodeIdentity ─────────────────────────────────────────────────────────────

NodeIdentity NodeIdentity::load_or_generate(const fs::path& dir) {
    if (sodium_init() < 0)
        throw std::runtime_error("libsodium init failed");

    NodeIdentity id{};

    fs::path user_p = dir / "user_key";
    if (load_key(user_p, id.user_seckey, sizeof(id.user_seckey))) {
        crypto_sign_ed25519_sk_to_pk(id.user_pubkey, id.user_seckey);
        LOG_INFO("[Identity] user key loaded");
    } else {
        crypto_sign_keypair(id.user_pubkey, id.user_seckey);
        save_key(user_p, id.user_seckey, sizeof(id.user_seckey));
        LOG_INFO("[Identity] user key generated → {}", user_p.string());
    }

    fs::path dev_p = dir / "device_key";
    if (load_key(dev_p, id.device_seckey, sizeof(id.device_seckey))) {
        crypto_sign_ed25519_sk_to_pk(id.device_pubkey, id.device_seckey);
        LOG_INFO("[Identity] device key loaded");
    } else {
        crypto_sign_keypair(id.device_pubkey, id.device_seckey);
        save_key(dev_p, id.device_seckey, sizeof(id.device_seckey));
        LOG_INFO("[Identity] device key generated → {}", dev_p.string());
    }

    LOG_INFO("[Identity] user  pubkey: {}", id.user_pubkey_hex());
    LOG_INFO("[Identity] device pubkey: {}", id.device_pubkey_hex());
    return id;
}

std::string NodeIdentity::user_pubkey_hex() const {
    return bytes_to_hex(user_pubkey, sizeof(user_pubkey));
}
std::string NodeIdentity::device_pubkey_hex() const {
    return bytes_to_hex(device_pubkey, sizeof(device_pubkey));
}

// ─── ConnectionManager ────────────────────────────────────────────────────────

ConnectionManager::ConnectionManager(PacketSignal& signal, NodeIdentity identity)
    : signal_(signal), identity_(std::move(identity))
{
    LOG_INFO("[ConnMgr] ready. user={}", identity_.user_pubkey_hex());
}

void ConnectionManager::register_connector(const std::string& scheme,
                                            connector_ops_t* ops) {
    std::unique_lock lock(connectors_mu_);
    connectors_[scheme] = ops;
    LOG_INFO("[ConnMgr] connector registered: {}", scheme);
}

void ConnectionManager::fill_host_api(host_api_t* api) {
    api->ctx               = this;
    api->on_connect        = &s_on_connect;
    api->on_data           = &s_on_data;
    api->on_disconnect     = &s_on_disconnect;
    api->send              = &s_send;
    api->sign_with_device  = &s_sign;
    api->verify_signature  = &s_verify;

    LOG_INFO("API check: on_connect={}, ctx={}", (void*)api->on_connect, api->ctx);
}

size_t ConnectionManager::connection_count() const {
    std::shared_lock lock(records_mu_);
    return records_.size();
}

std::vector<std::string> ConnectionManager::get_active_uris() const {
    std::shared_lock lock(records_mu_);
    std::vector<std::string> uris;
    for (auto const& [id, rec] : records_) {
        uris.push_back(fmt::format("{}://{}:{}", rec.scheme, rec.remote.address, rec.remote.port));
    }
    return uris;
}

std::optional<conn_state_t> ConnectionManager::get_state(conn_id_t id) const {
    std::shared_lock lock(records_mu_);
    auto it = records_.find(id);
    if (it == records_.end()) return std::nullopt;
    return it->second.state;
}

// ─── on_connect ───────────────────────────────────────────────────────────────

conn_id_t ConnectionManager::handle_connect(const endpoint_t* ep) {
    conn_id_t id = next_id_.fetch_add(1, std::memory_order_relaxed);

    {
        std::unique_lock lock(records_mu_);
        ConnectionRecord rec;
        rec.id     = id;
        rec.state  = STATE_AUTH_PENDING;
        rec.remote = *ep;
        records_.emplace(id, std::move(rec));
    }

    {
        std::unique_lock lock(uri_mu_);
        uri_index_[fmt::format("{}:{}", ep->address, ep->port)] = id;
    }

    LOG_INFO("[ConnMgr] connect id={} from {}:{}", id, ep->address, ep->port);

    // AUTH отправляется снаружи lock'ов
    send_auth(id);
    return id;
}

// ─── on_data ─────────────────────────────────────────────────────────────────
//
// Ключевой момент:
//   handle_data() берёт write-lock на records_mu_ только для добавления байт
//   в recv_buf и сборки пакета.
//   process_auth() и dispatch_packet() вызываются уже без lock'а —
//   они берут свои локи только для нужных структур.
//   Это устраняет deadlock: process_auth() пытался взять records_mu_
//   пока handle_data() уже держал его.

void ConnectionManager::handle_data(conn_id_t id, const void* raw, size_t size) {
    std::vector<std::pair<header_t, std::vector<uint8_t>>> packets_to_dispatch;

    {
        std::unique_lock lock(records_mu_);
        auto it = records_.find(id);
        if (it == records_.end()) return;

        // 1. Добавляем новые данные
        const auto* bytes = static_cast<const uint8_t*>(raw);
        it->second.recv_buf.insert(it->second.recv_buf.end(), bytes, bytes + size);

        // 2. Сразу вырезаем все полные пакеты из буфера под этим же локом
        auto& buf = it->second.recv_buf;
        size_t consumed = 0;

        while (buf.size() - consumed >= sizeof(header_t)) {
            const auto* hdr = reinterpret_cast<const header_t*>(buf.data() + consumed);
            
            // Проверка magic/proto (упрощенно)
            if (hdr->magic != GNET_MAGIC) { 
                buf.clear(); return; 
            }

            size_t total = sizeof(header_t) + hdr->payload_len;
            if (buf.size() - consumed < total) break;

            // Копируем пакет для обработки вне лока
            header_t hdr_copy = *hdr;
            std::vector<uint8_t> payload(
                buf.begin() + consumed + sizeof(header_t),
                buf.begin() + consumed + total
            );
            
            packets_to_dispatch.emplace_back(hdr_copy, std::move(payload));
            consumed += total;
        }

        if (consumed > 0) {
            buf.erase(buf.begin(), buf.begin() + consumed);
        }
    }

    // 3. Рассылаем пакеты БЕЗ лока
    for (auto& [hdr, payload] : packets_to_dispatch) {
        dispatch_packet(id, &hdr, payload.data(), payload.size());
    }
}

// ─── dispatch_packet ─────────────────────────────────────────────────────────
//
// Вызывается БЕЗ records_mu_. Берёт свои локи точечно.

void ConnectionManager::dispatch_packet(conn_id_t id,
                                         const header_t* hdr,
                                         const uint8_t* payload,
                                         size_t payload_size) {
    if (hdr->payload_type == MSG_TYPE_AUTH) {
        process_auth(id, payload, payload_size);
        return;
    }

    // Проверяем состояние под shared_lock
    conn_state_t state;
    endpoint_t   remote{};
    {
        std::shared_lock lock(records_mu_);
        auto it = records_.find(id);
        if (it == records_.end()) return;
        state  = it->second.state;
        remote = it->second.remote;
    }

    if (state != STATE_ESTABLISHED) {
        LOG_DEBUG("[ConnMgr] drop packet type={} on non-established id={}",
                  hdr->payload_type, id);
        return;
    }

    auto hdr_copy = std::make_shared<header_t>(*hdr);
    auto pl_copy  = std::make_shared<std::vector<char>>(
        reinterpret_cast<const char*>(payload),
        reinterpret_cast<const char*>(payload) + payload_size);

    signal_.emit(hdr_copy, &remote, pl_copy);
}

// ─── process_auth ────────────────────────────────────────────────────────────
//
// Вызывается БЕЗ records_mu_. Использует отдельные локи для records_ и pk_index_.

bool ConnectionManager::process_auth(conn_id_t id,
                                      const uint8_t* payload, size_t size) {
    if (size < sizeof(auth_payload_t)) {
        LOG_WARN("[ConnMgr] AUTH: payload too short id={}", id);
        return false;
    }

    auth_payload_t ap{};
    std::memcpy(&ap, payload, sizeof(ap));

    uint8_t to_verify[64];
    std::memcpy(to_verify,      ap.user_pubkey,   32);
    std::memcpy(to_verify + 32, ap.device_pubkey, 32);

    if (crypto_sign_verify_detached(ap.signature,
                                    to_verify, sizeof(to_verify),
                                    ap.user_pubkey) != 0) {
        LOG_WARN("[ConnMgr] AUTH: bad signature id={}", id);
        return false;
    }

    // Обновляем запись
    {
        std::unique_lock lock(records_mu_);
        auto it = records_.find(id);
        if (it == records_.end()) return false;
        std::memcpy(it->second.peer_user_pubkey,   ap.user_pubkey,   32);
        std::memcpy(it->second.peer_device_pubkey, ap.device_pubkey, 32);
        it->second.peer_authenticated = true;
        it->second.state = STATE_ESTABLISHED;
    }

    // pk_index под своим локом
    std::string pk_hex = bytes_to_hex(ap.user_pubkey, 32);
    {
        std::unique_lock lock(pk_mu_);
        pk_index_[pk_hex] = id;
    }

    LOG_INFO("[ConnMgr] AUTH ok id={} peer={}...", id, pk_hex.substr(0, 16));
    return true;
}

// ─── send_auth ────────────────────────────────────────────────────────────────
//
// Отправляет наш AUTH-пакет. Вызывается после handle_connect,
// когда records_mu_ уже отпущен.

void ConnectionManager::send_auth(conn_id_t id) {
    auth_payload_t ap{};
    std::memcpy(ap.user_pubkey,   identity_.user_pubkey,   32);
    std::memcpy(ap.device_pubkey, identity_.device_pubkey, 32);

    uint8_t to_sign[64];
    std::memcpy(to_sign,      ap.user_pubkey,   32);
    std::memcpy(to_sign + 32, ap.device_pubkey, 32);

    crypto_sign_detached(ap.signature, nullptr,
                         to_sign, sizeof(to_sign),
                         identity_.user_seckey);

    header_t hdr{};
    hdr.magic        = GNET_MAGIC;
    hdr.proto_ver    = GNET_PROTO_VER;
    hdr.payload_type = MSG_TYPE_AUTH;
    hdr.payload_len  = sizeof(ap);

    std::vector<uint8_t> wire(sizeof(hdr) + sizeof(ap));
    std::memcpy(wire.data(), &hdr, sizeof(hdr));
    std::memcpy(wire.data() + sizeof(hdr), &ap, sizeof(ap));

    // scheme под shared_lock — не блокирует другие shared_lock
    std::string scheme;
    {
        std::shared_lock lock(records_mu_);
        auto it = records_.find(id);
        if (it == records_.end()) return;
        scheme = it->second.scheme;
    }
    if (scheme.empty()) scheme = "tcp";

    connector_ops_t* ops = find_connector(scheme);
    if (ops) {
        ops->send_to(ops->connector_ctx, id, wire.data(), wire.size());
        LOG_DEBUG("[ConnMgr] AUTH sent to id={}", id);
    }
}

// ─── on_disconnect ────────────────────────────────────────────────────────────

void ConnectionManager::handle_disconnect(conn_id_t id, int error) {
    std::string uri_key;
    std::string pk_hex;

    {
        std::unique_lock lock(records_mu_);
        auto it = records_.find(id);
        if (it != records_.end()) {
            uri_key = fmt::format("{}:{}", it->second.remote.address,
                                           it->second.remote.port);
            if (it->second.peer_authenticated)
                pk_hex = bytes_to_hex(it->second.peer_user_pubkey, 32);
            records_.erase(it);
        }
    }
    if (!uri_key.empty()) {
        std::unique_lock lock(uri_mu_);
        uri_index_.erase(uri_key);
    }
    if (!pk_hex.empty()) {
        std::unique_lock lock(pk_mu_);
        pk_index_.erase(pk_hex);
    }
    LOG_INFO("[ConnMgr] disconnected id={} err={}", id, error);
}

// ─── Исходящая отправка ───────────────────────────────────────────────────────

void ConnectionManager::send(const char* uri, uint32_t msg_type,
                              const void* payload, size_t payload_size) {
    if (!uri) return;

    auto id_opt = resolve_uri(uri);
    if (!id_opt) {
        LOG_ERROR("[ConnMgr] send: can't resolve '{}'", uri);
        return;
    }
    conn_id_t id = *id_opt;

    header_t hdr{};
    hdr.magic        = GNET_MAGIC;
    hdr.proto_ver    = GNET_PROTO_VER;
    hdr.payload_type = msg_type;
    hdr.payload_len  = static_cast<uint32_t>(payload_size);

    std::vector<uint8_t> wire(sizeof(hdr) + payload_size);
    std::memcpy(wire.data(), &hdr, sizeof(hdr));
    if (payload && payload_size)
        std::memcpy(wire.data() + sizeof(hdr), payload, payload_size);

    std::string scheme;
    {
        std::shared_lock lock(records_mu_);
        auto it = records_.find(id);
        if (it == records_.end()) return;
        scheme = it->second.scheme;
    }
    if (scheme.empty()) scheme = "tcp";

    connector_ops_t* ops = find_connector(scheme);
    if (!ops) { LOG_ERROR("[ConnMgr] no connector for '{}'", scheme); return; }
    ops->send_to(ops->connector_ctx, id, wire.data(), wire.size());
}

std::optional<conn_id_t>
ConnectionManager::resolve_uri(const std::string& uri) {
    // 1. uri_index: "host:port"
    {
        auto sep = uri.find("://");
        std::string key = (sep != std::string::npos) ? uri.substr(sep + 3) : uri;
        std::shared_lock lock(uri_mu_);
        auto it = uri_index_.find(key);
        if (it != uri_index_.end()) return it->second;
    }

    // 2. gn://pubkey_hex
    if (uri.size() > 5 && uri.substr(0, 5) == "gn://") {
        std::string hex = uri.substr(5);
        std::shared_lock lock(pk_mu_);
        auto it = pk_index_.find(hex);
        if (it != pk_index_.end()) return it->second;
        return std::nullopt;
    }

    // 3. Новое соединение
    auto sep = uri.find("://");
    if (sep == std::string::npos) return std::nullopt;
    std::string scheme = uri.substr(0, sep);
    connector_ops_t* ops = find_connector(scheme);
    if (!ops || ops->connect(ops->connector_ctx, uri.c_str()) != 0)
        return std::nullopt;

    std::string key = uri.substr(sep + 3);
    std::shared_lock lock(uri_mu_);
    auto it = uri_index_.find(key);
    return (it != uri_index_.end()) ? std::optional{it->second} : std::nullopt;
}

connector_ops_t* ConnectionManager::find_connector(const std::string& scheme) {
    std::shared_lock lock(connectors_mu_);
    auto it = connectors_.find(scheme);
    return (it != connectors_.end()) ? it->second : nullptr;
}

// ─── Статические адаптеры ─────────────────────────────────────────────────────

conn_id_t ConnectionManager::s_on_connect(void* ctx, const endpoint_t* ep) {
    return static_cast<ConnectionManager*>(ctx)->handle_connect(ep);
}
void ConnectionManager::s_on_data(void* ctx, conn_id_t id,
                                   const void* raw, size_t size) {
    static_cast<ConnectionManager*>(ctx)->handle_data(id, raw, size);
}
void ConnectionManager::s_on_disconnect(void* ctx, conn_id_t id, int err) {
    static_cast<ConnectionManager*>(ctx)->handle_disconnect(id, err);
}
void ConnectionManager::s_send(void* ctx, const char* uri,
                                uint32_t type, const void* pl, size_t sz) {
    static_cast<ConnectionManager*>(ctx)->send(uri, type, pl, sz);
}
int ConnectionManager::s_sign(void* ctx, const void* data, size_t size,
                               uint8_t sig[64]) {
    auto* self = static_cast<ConnectionManager*>(ctx);
    return crypto_sign_detached(
        sig, nullptr,
        static_cast<const unsigned char*>(data), size,
        self->identity_.device_seckey);
}
int ConnectionManager::s_verify(void* ctx, const void* data, size_t dsz,
                                 const uint8_t* pk, const uint8_t* sig) {
    (void)ctx;
    return crypto_sign_verify_detached(
        sig, static_cast<const unsigned char*>(data), dsz, pk);
}

} // namespace gn
