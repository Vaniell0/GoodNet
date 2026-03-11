#include "connectionManager.hpp"
#include "logger.hpp"
#include <cstring>
#include <cstddef>
#include <algorithm>

namespace gn {

std::string bytes_to_hex(const uint8_t* data, size_t len);

size_t ConnectionManager::connection_count() const {
    std::shared_lock lk(records_mu_); return records_.size();
}

std::vector<std::string> ConnectionManager::get_active_uris() const {
    std::shared_lock lk(uri_mu_);
    std::vector<std::string> out; out.reserve(uri_index_.size());
    for (const auto& [uri, _] : uri_index_) out.push_back(uri);
    return out;
}

std::optional<conn_state_t> ConnectionManager::get_state(conn_id_t id) const {
    std::shared_lock lk(records_mu_);
    if (auto it = records_.find(id); it != records_.end()) return it->second.state;
    return std::nullopt;
}

std::optional<std::string> ConnectionManager::get_negotiated_scheme(conn_id_t id) const {
    std::shared_lock lk(records_mu_);
    if (auto it = records_.find(id); it != records_.end())
        return it->second.negotiated_scheme;
    return std::nullopt;
}

bool ConnectionManager::is_localhost_address(std::string_view a) {
    return a == "127.0.0.1" || a == "::1" || a == "localhost" || a.starts_with("127.");
}

connector_ops_t* ConnectionManager::find_connector(const std::string& scheme) {
    std::shared_lock lk(connectors_mu_);
    if (auto it = connectors_.find(scheme); it != connectors_.end()) return it->second;
    return nullptr;
}

std::optional<conn_id_t> ConnectionManager::resolve_uri(const std::string& uri) {
    std::string key = uri;
    if (const auto sep = key.find("://"); sep != std::string::npos)
        key = key.substr(sep + 3);
    std::shared_lock lk(uri_mu_);
    if (auto it = uri_index_.find(key); it != uri_index_.end()) return it->second;
    return std::nullopt;
}

std::vector<std::string> ConnectionManager::local_schemes() const {
    std::shared_lock lk(connectors_mu_);
    std::vector<std::string> out; out.reserve(connectors_.size());
    for (const auto& [s, _] : connectors_) out.push_back(s);
    return out;
}

/// @brief Pick best scheme available on both sides, respecting scheme_priority_ order.
std::string ConnectionManager::negotiate_scheme(const ConnectionRecord& rec) const {
    const auto local = local_schemes();
    for (const auto& prio : scheme_priority_) {
        if (std::find(local.begin(), local.end(), prio) == local.end()) continue;
        if (rec.peer_schemes.empty()) return prio;
        if (std::find(rec.peer_schemes.begin(), rec.peer_schemes.end(), prio)
                != rec.peer_schemes.end())
            return prio;
    }
    return local.empty() ? "tcp" : local.front();
}

// ─── send_frame ──────────────────────────────────────────────────────────────
// Отправляет один фрейм (без изменений, только мелкие правки для читаемости)

/// @brief Pack and send a single frame. Encrypts payload for non-localhost ESTABLISHED connections.
void ConnectionManager::send_frame(conn_id_t id, uint32_t msg_type,
                                   const void* payload, size_t payload_size) {
    std::string scheme;
    bool is_localhost = false;
    SessionState* session_ptr = nullptr;

    {
        std::shared_lock lk(records_mu_);
        auto it = records_.find(id);
        if (it == records_.end()) return;
        const auto& rec = it->second;
        scheme = rec.negotiated_scheme.empty() ? rec.local_scheme : rec.negotiated_scheme;
        is_localhost = rec.is_localhost;
        session_ptr = rec.session.get();
    }

    auto* ops = find_connector(scheme.empty() ? "tcp" : scheme);
    if (!ops || !ops->send_to) {
        LOG_WARN("send_frame #{}: no connector for scheme '{}'", id, scheme);
        return;
    }

    std::vector<uint8_t> encrypted;
    const void* final_payload = payload;
    size_t final_size = payload_size;

    const bool do_encrypt = (msg_type != MSG_TYPE_AUTH) &&
                            !is_localhost &&
                            session_ptr &&
                            session_ptr->ready;

    if (do_encrypt) {
        encrypted = session_ptr->encrypt(payload, payload_size);
        final_payload = encrypted.data();
        final_size = encrypted.size();
    }

    header_t hdr{};
    hdr.magic = GNET_MAGIC;
    hdr.proto_ver = GNET_PROTO_VER;
    hdr.payload_type = msg_type;
    hdr.payload_len = static_cast<uint32_t>(final_size);
    hdr.status = STATUS_OK;

    if (msg_type != MSG_TYPE_AUTH && !is_localhost) {
        const size_t hdr_body = offsetof(header_t, signature);
        crypto_sign_ed25519_detached(
            hdr.signature, nullptr,
            reinterpret_cast<const uint8_t*>(&hdr), hdr_body,
            identity_.device_seckey);
    }

    std::vector<uint8_t> frame(sizeof(header_t) + final_size);
    std::memcpy(frame.data(), &hdr, sizeof(header_t));
    if (final_size && final_payload)
        std::memcpy(frame.data() + sizeof(header_t), final_payload, final_size);

    ops->send_to(ops->connector_ctx, id, frame.data(), frame.size());
}

// ─── send ────────────────────────────────────────────────────────────────────
// Основная точка входа: проверяет очередь, разбивает на чанки
void ConnectionManager::send(const char* uri, uint32_t msg_type,
                             const void* payload, size_t size) {
    if (!uri) return;

    const std::string uri_str(uri);
    auto conn_id_opt = resolve_uri(uri_str);

    if (!conn_id_opt) {
        // Пытаемся подключиться
        const auto sep = uri_str.find("://");
        const std::string scheme = (sep != std::string::npos) ? uri_str.substr(0, sep) : "tcp";
        if (auto* ops = find_connector(scheme)) {
            ops->connect(ops->connector_ctx, uri_str.c_str());
        }
        return;
    }

    const conn_id_t conn_id = *conn_id_opt;

    // Проверяем состояние
    {
        std::shared_lock lk(records_mu_);
        auto it = records_.find(conn_id);
        if (it == records_.end()) return;

        // РАЗРЕШАЕМ системные сообщения (тип 0 - Handshake, AUTH и т.д.),
        // даже если статус еще не ESTABLISHED
        bool is_system = (msg_type == 0 || msg_type == 1 /* MSG_TYPE_AUTH */);
        if (it->second.state != STATE_ESTABLISHED && !is_system) return;
    }

    // Memory Guard + Chunking (как раньше)
    if (pending_bytes_.load(std::memory_order_relaxed) + size > MAX_IN_FLIGHT_BYTES) {
        LOG_WARN("Backpressure: queue full, dropping");
        return;
    }
    pending_bytes_.fetch_add(size);

    if (size > CHUNK_SIZE * 2) {
        const uint8_t* ptr = static_cast<const uint8_t*>(payload);
        size_t offset = 0;
        while (offset < size) {
            size_t chunk = std::min(CHUNK_SIZE, size - offset);
            send_frame(conn_id, msg_type, ptr + offset, chunk);
            offset += chunk;
        }
        pending_bytes_.fetch_sub(size);
        return;
    }

    send_frame(conn_id, msg_type, payload, size);
    pending_bytes_.fetch_sub(size);
}

} // namespace gn