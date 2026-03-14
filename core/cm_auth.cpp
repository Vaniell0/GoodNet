/// @file core/cm_auth.cpp
/// Authentication protocol + key rotation.

#include "connectionManager.hpp"
#include "logger.hpp"

#include <cstring>

#include <sodium/crypto_box.h>
#include <sodium/crypto_sign.h>

namespace gn {

std::string bytes_to_hex(const uint8_t* data, size_t len);

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

} // namespace gn
