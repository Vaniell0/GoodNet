/// @file core/cm_handshake.cpp
/// Noise_XX handshake + key rotation + rekey.

#include "cm_impl.hpp"
#include "logger.hpp"

#include <cstring>

#include <sodium/crypto_sign.h>
#include <sodium/utils.h>

#include "../sdk/connector.h"
#include "util.hpp"

namespace gn {

// ═══════════════════════════════════════════════════════════════════════════════
// Key rotation
// ═══════════════════════════════════════════════════════════════════════════════

void ConnectionManager::Impl::rotate_identity_keys(const IdentityConfig& cfg) {
    LOG_SCOPE_TRACE();

    NodeIdentity next = NodeIdentity::load_or_generate(cfg);
    std::unique_lock lk(identity_mu_);
    identity_ = std::move(next);
    LOG_INFO("Identity rotated — user={}...", bytes_to_hex(identity_.user_pubkey, 4));
}

bool ConnectionManager::Impl::rekey_session(conn_id_t id) {
    LOG_SCOPE_DEBUG();

    auto rec = rcu_find(id);
    if (!rec || !rec->session || rec->state != STATE_ESTABLISHED) return false;

    // Noise native rekey — no messages needed, just update keys locally.
    // Обе стороны должны вызвать rekey синхронно (по таймеру или счётчику).
    uint8_t new_send[noise::KEYLEN], new_recv[noise::KEYLEN];
    noise::hkdf2(rec->session->send_key, nullptr, 0, new_send, new_recv);
    std::memcpy(rec->session->send_key, new_send, noise::KEYLEN);
    sodium_memzero(new_send, sizeof(new_send));

    noise::hkdf2(rec->session->recv_key, nullptr, 0, new_recv, new_send);
    std::memcpy(rec->session->recv_key, new_recv, noise::KEYLEN);
    sodium_memzero(new_recv, sizeof(new_recv));
    sodium_memzero(new_send, sizeof(new_send));

    rec->session->recv_window.reset();
    rec->send_packet_id.store(1, std::memory_order_release);

    LOG_INFO("rekey_session #{}: Noise native rekey done", id);
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Noise_XX handshake
// ═══════════════════════════════════════════════════════════════════════════════

void ConnectionManager::Impl::send_noise_init(conn_id_t id) {
    LOG_SCOPE_TRACE();

    auto rec = rcu_find(id);
    if (!rec || !rec->handshake) return;

    // msg1: → e (no payload)
    uint8_t buf[256];
    size_t  out_len = 0;
    if (!rec->handshake->write_message(nullptr, 0, buf, &out_len)) {
        LOG_ERROR("send_noise_init #{}: write_message failed", id);
        close_now(id);
        return;
    }

    send_frame(id, MSG_TYPE_NOISE_INIT,
               std::span<const uint8_t>(buf, out_len));
    LOG_DEBUG("send_noise_init #{}: sent {} bytes", id, out_len);
}

std::vector<uint8_t> ConnectionManager::Impl::build_handshake_payload() {
    std::shared_lock lk(identity_mu_);

    msg::HandshakePayload hp{};
    std::memcpy(hp.user_pubkey,   identity_.user_pubkey,   GN_SIGN_PUBLICKEYBYTES);
    std::memcpy(hp.device_pubkey, identity_.device_pubkey, GN_SIGN_PUBLICKEYBYTES);

    // Подпись: sig = Ed25519(user_sk, user_pk || device_pk)
    uint8_t to_sign[GN_SIGN_PUBLICKEYBYTES * 2];
    std::memcpy(to_sign,                          hp.user_pubkey,   GN_SIGN_PUBLICKEYBYTES);
    std::memcpy(to_sign + GN_SIGN_PUBLICKEYBYTES, hp.device_pubkey, GN_SIGN_PUBLICKEYBYTES);
    crypto_sign_ed25519_detached(hp.signature, nullptr,
                                  to_sign, sizeof(to_sign),
                                  identity_.user_seckey);

    hp.set_schemes(local_schemes());
    hp.core_meta = local_core_meta();

    return std::vector<uint8_t>(
        reinterpret_cast<const uint8_t*>(&hp),
        reinterpret_cast<const uint8_t*>(&hp) + sizeof(hp));
}

bool ConnectionManager::Impl::process_handshake_payload(conn_id_t id,
                                                         const uint8_t* data,
                                                         size_t len) {
    if (len < sizeof(msg::HandshakePayload)) {
        LOG_WARN("process_handshake_payload #{}: too short ({} < {})",
                 id, len, sizeof(msg::HandshakePayload));
        bus_.emit_drop(id, DropReason::AuthFail);
        return false;
    }

    const auto* hp = reinterpret_cast<const msg::HandshakePayload*>(data);

    // Проверка подписи: user_pk || device_pk
    uint8_t to_verify[GN_SIGN_PUBLICKEYBYTES * 2];
    std::memcpy(to_verify,                          hp->user_pubkey,   GN_SIGN_PUBLICKEYBYTES);
    std::memcpy(to_verify + GN_SIGN_PUBLICKEYBYTES, hp->device_pubkey, GN_SIGN_PUBLICKEYBYTES);
    if (crypto_sign_ed25519_verify_detached(hp->signature, to_verify,
                                             sizeof(to_verify),
                                             hp->user_pubkey) != 0) {
        LOG_WARN("process_handshake_payload #{}: invalid signature", id);
        bus_.emit_drop(id, DropReason::AuthFail);
        return false;
    }

    // Верификация: device_pubkey Ed25519 → X25519 должен совпадать с rs из Noise
    auto rec = rcu_find(id);
    if (!rec || !rec->handshake) return false;

    uint8_t expected_x25519[32];
    if (crypto_sign_ed25519_pk_to_curve25519(expected_x25519, hp->device_pubkey) != 0) {
        LOG_WARN("process_handshake_payload #{}: Ed25519→X25519 conversion failed", id);
        bus_.emit_drop(id, DropReason::AuthFail);
        return false;
    }
    if (std::memcmp(expected_x25519, rec->handshake->rs, noise::DHLEN) != 0) {
        LOG_WARN("process_handshake_payload #{}: device_pubkey ≠ Noise static key", id);
        bus_.emit_drop(id, DropReason::AuthFail);
        return false;
    }

    const auto peer_schemes = hp->get_schemes();
    const auto peer_meta    = hp->core_meta;

    {
        std::lock_guard wlk(records_write_mu_);
        rcu_update([&](RecordMap& m) {
            auto it = m.find(id);
            if (it == m.end()) return;
            auto& r = *it->second;
            std::memcpy(r.peer_user_pubkey,   hp->user_pubkey,   GN_SIGN_PUBLICKEYBYTES);
            std::memcpy(r.peer_device_pubkey, hp->device_pubkey, GN_SIGN_PUBLICKEYBYTES);
            r.peer_authenticated = true;
            r.peer_schemes       = peer_schemes;
            r.peer_core_meta     = peer_meta;
            r.negotiated_scheme  = negotiate_scheme(r);

            // Обновляем первичный TransportPath после negotiate
            if (!r.transport_paths.empty()) {
                auto& tp = r.transport_paths[0];
                tp.scheme   = r.negotiated_scheme.empty() ? "tcp" : r.negotiated_scheme;
                tp.priority = scheme_priority_index(tp.scheme);
            }
        });
    }

    return true;
}

void ConnectionManager::Impl::finalize_handshake(conn_id_t id) {
    auto rec = rcu_find(id);
    if (!rec || !rec->handshake) return;

    // Проверяем дубликат: если к этому пиру уже есть ESTABLISHED соединение
    {
        const std::string pk_hex = bytes_to_hex(rec->peer_user_pubkey,
                                                 crypto_sign_PUBLICKEYBYTES);
        std::shared_lock lk(pk_mu_);
        auto it = pk_index_.find(pk_hex);
        if (it != pk_index_.end() && it->second != id) {
            const conn_id_t existing = it->second;
            lk.unlock();
            LOG_INFO("finalize_handshake #{}: duplicate peer (existing #{}), closing",
                     id, existing);
            // Закрываем транспорт и удаляем запись (хэндшейк не завершён → нет pk/handler cleanup)
            close_now(id);
            {
                std::lock_guard wlk(records_write_mu_);
                rcu_update([&](RecordMap& m) { m.erase(id); });
            }
            { std::unique_lock lk2(queues_mu_); send_queues_.erase(id); }
            {
                const std::string uri_key = std::string(rec->remote.address) + ":"
                                          + std::to_string(rec->remote.port);
                std::unique_lock lk2(uri_mu_);
                uri_index_.erase(uri_key);
            }
            return;
        }
    }

    // Split → transport keys
    noise::CipherState c_send, c_recv;
    rec->handshake->split(c_send, c_recv);

    auto session = std::make_unique<NoiseSession>();
    std::memcpy(session->send_key, c_send.key, noise::KEYLEN);
    std::memcpy(session->recv_key, c_recv.key, noise::KEYLEN);
    rec->handshake->get_handshake_hash(session->handshake_hash);

    c_send.clear();
    c_recv.clear();

    {
        std::lock_guard wlk(records_write_mu_);
        rcu_update([&](RecordMap& m) {
            auto it = m.find(id);
            if (it == m.end()) return;
            auto& r = *it->second;
            r.session = std::move(session);
            r.handshake.reset();
            r.state = STATE_ESTABLISHED;
            if (r.is_localhost)
                r.localhost_passthrough = true;
        });
        std::unique_lock lk(pk_mu_);
        pk_index_[bytes_to_hex(rec->peer_user_pubkey, crypto_sign_PUBLICKEYBYTES)] = id;
    }

    LOG_INFO("Noise_XX #{}: peer={}... scheme='{}' → ESTABLISHED",
             id, bytes_to_hex(rec->peer_user_pubkey, 4),
             rec->negotiated_scheme.empty() ? "?" : rec->negotiated_scheme);

    // Flush pending messages для этого URI
    const std::string uri_key = std::string(rec->remote.address) + ":"
                              + std::to_string(rec->remote.port);
    flush_pending_messages(uri_key, id);

    {
        const std::string uri = bytes_to_hex(rec->peer_user_pubkey, crypto_sign_PUBLICKEYBYTES);
        std::shared_lock lk(handlers_mu_);
        for (auto& [name, entry] : handler_entries_)
            if (entry.handler && entry.handler->handle_conn_state)
                entry.handler->handle_conn_state(entry.handler->user_data,
                                                  uri.c_str(), STATE_ESTABLISHED);
    }
    bus_.emit_stat({StatsEvent::Kind::AuthOk, 1, id});
    bus_.on_conn_state.emit(id, STATE_ESTABLISHED);

    // Инициатор пытается upgrade на лучший транспорт
    if (rec->is_initiator)
        schedule_transport_upgrade(id);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Transport upgrade
// ═══════════════════════════════════════════════════════════════════════════════

void ConnectionManager::Impl::schedule_transport_upgrade(conn_id_t id) {
    auto rec = rcu_find(id);
    if (!rec || rec->state != STATE_ESTABLISHED) return;

    const auto* current_best = rec->best_path();
    if (!current_best) return;

    const auto peer_caps = rec->peer_core_meta.caps_mask;
    const std::string pk_hex = bytes_to_hex(rec->peer_user_pubkey,
                                             crypto_sign_PUBLICKEYBYTES);

    for (const auto& scheme : scheme_priority_) {
        // Если текущий лучший путь уже на этом scheme — уже на лучшем
        if (current_best->scheme == scheme) break;

        // Проверяем: есть ли коннектор для этого scheme
        auto* ops = find_connector(scheme);
        if (!ops) continue;

        // Проверяем: поддерживает ли пир этот scheme
        bool peer_supports = false;
        if (scheme == "ice") {
            peer_supports = (peer_caps & CORE_CAP_ICE) != 0;
        } else {
            for (const auto& ps : rec->peer_schemes)
                if (ps == scheme) { peer_supports = true; break; }
        }
        if (!peer_supports) continue;

        // Проверяем: нет ли уже пути с этим scheme
        if (rec->find_path(scheme)) continue;

        // Инициируем upgrade через коннектор
        const std::string uri = scheme + "://" + pk_hex;
        LOG_INFO("schedule_transport_upgrade #{}: {} → trying '{}'",
                 id, current_best->scheme, scheme);
        ops->connect(ops->connector_ctx, uri.c_str());
        break;  // Одна попытка за раз
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// handle_noise_init / resp / fin
// ═══════════════════════════════════════════════════════════════════════════════

void ConnectionManager::Impl::handle_noise_init(conn_id_t id,
                                                  std::span<const uint8_t> payload) {
    LOG_SCOPE_DEBUG();

    auto rec = rcu_find(id);
    if (!rec || !rec->handshake) {
        LOG_WARN("handle_noise_init #{}: no handshake state", id);
        return;
    }
    if (rec->handshake->initiator) {
        LOG_WARN("handle_noise_init #{}: received INIT but we are initiator", id);
        return;
    }

    // Responder reads msg1 (→ e)
    uint8_t payload_out[1];
    size_t  payload_len = 0;
    if (!rec->handshake->read_message(payload.data(), payload.size(),
                                       payload_out, &payload_len)) {
        LOG_WARN("handle_noise_init #{}: read_message failed", id);
        bus_.emit_drop(id, DropReason::AuthFail);
        close_now(id);
        return;
    }

    // Responder writes msg2 (← e, ee, s, es) with HandshakePayload
    auto hp_bytes = build_handshake_payload();

    std::vector<uint8_t> msg2(noise::DHLEN * 2 + noise::MACLEN * 2 + hp_bytes.size() + 256);
    size_t msg2_len = 0;
    if (!rec->handshake->write_message(hp_bytes.data(), hp_bytes.size(),
                                        msg2.data(), &msg2_len)) {
        LOG_WARN("handle_noise_init #{}: write_message msg2 failed", id);
        close_now(id);
        return;
    }
    msg2.resize(msg2_len);

    send_frame(id, MSG_TYPE_NOISE_RESP,
               std::span<const uint8_t>(msg2));
    LOG_DEBUG("handle_noise_init #{}: sent NOISE_RESP ({} bytes)", id, msg2_len);
}

void ConnectionManager::Impl::handle_noise_resp(conn_id_t id,
                                                  std::span<const uint8_t> payload) {
    LOG_SCOPE_DEBUG();

    auto rec = rcu_find(id);
    if (!rec || !rec->handshake) {
        LOG_WARN("handle_noise_resp #{}: no handshake state", id);
        return;
    }
    if (!rec->handshake->initiator) {
        LOG_WARN("handle_noise_resp #{}: received RESP but we are responder", id);
        return;
    }

    // Initiator reads msg2 (← e, ee, s, es)
    std::vector<uint8_t> payload_out(payload.size());
    size_t payload_len = 0;
    if (!rec->handshake->read_message(payload.data(), payload.size(),
                                       payload_out.data(), &payload_len)) {
        LOG_WARN("handle_noise_resp #{}: read_message failed", id);
        bus_.emit_drop(id, DropReason::AuthFail);
        close_now(id);
        return;
    }

    if (!process_handshake_payload(id, payload_out.data(), payload_len)) {
        close_now(id);
        return;
    }

    // Initiator writes msg3 (→ s, se)
    auto hp_bytes = build_handshake_payload();

    std::vector<uint8_t> msg3(noise::DHLEN + noise::MACLEN * 2 + hp_bytes.size() + 256);
    size_t msg3_len = 0;
    if (!rec->handshake->write_message(hp_bytes.data(), hp_bytes.size(),
                                        msg3.data(), &msg3_len)) {
        LOG_WARN("handle_noise_resp #{}: write_message msg3 failed", id);
        close_now(id);
        return;
    }
    msg3.resize(msg3_len);

    send_frame(id, MSG_TYPE_NOISE_FIN,
               std::span<const uint8_t>(msg3));
    LOG_DEBUG("handle_noise_resp #{}: sent NOISE_FIN ({} bytes)", id, msg3_len);

    // Initiator done → finalize
    finalize_handshake(id);
}

void ConnectionManager::Impl::handle_noise_fin(conn_id_t id,
                                                 std::span<const uint8_t> payload) {
    LOG_SCOPE_DEBUG();

    auto rec = rcu_find(id);
    if (!rec || !rec->handshake) {
        LOG_WARN("handle_noise_fin #{}: no handshake state", id);
        return;
    }
    if (rec->handshake->initiator) {
        LOG_WARN("handle_noise_fin #{}: received FIN but we are initiator", id);
        return;
    }

    // Responder reads msg3 (→ s, se)
    std::vector<uint8_t> payload_out(payload.size());
    size_t payload_len = 0;
    if (!rec->handshake->read_message(payload.data(), payload.size(),
                                       payload_out.data(), &payload_len)) {
        LOG_WARN("handle_noise_fin #{}: read_message failed", id);
        bus_.emit_drop(id, DropReason::AuthFail);
        close_now(id);
        return;
    }

    if (!process_handshake_payload(id, payload_out.data(), payload_len)) {
        close_now(id);
        return;
    }

    // Responder done → finalize
    finalize_handshake(id);
}

} // namespace gn
