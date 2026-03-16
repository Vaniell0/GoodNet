#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <functional>

#include <sodium/utils.h>
#include <sodium/crypto_box.h>
#include <sodium/crypto_secretbox.h>

#include "data/messages.hpp"
#include "../sdk/handler.h"

namespace gn {

// ── SessionState ──────────────────────────────────────────────────────────────

/// Per-connection symmetric encryption state (XSalsa20-Poly1305).
/// Non-copyable, non-movable — owned exclusively via unique_ptr.
struct SessionState {
    uint8_t session_key[crypto_secretbox_KEYBYTES]{};
    bool    ready = false;

    uint8_t my_ephem_pk[crypto_box_PUBLICKEYBYTES]{};
    uint8_t my_ephem_sk[crypto_box_SECRETKEYBYTES]{};

    std::atomic<uint64_t> send_nonce{1};
    std::atomic<uint64_t> recv_nonce_expected{1};
    std::atomic<bool>     rekey_pending{false};

    std::vector<uint8_t> encrypt(const void* plain, size_t len);
    std::vector<uint8_t> decrypt(const void* wire,  size_t len);

    void clear_ephemeral() noexcept {
        sodium_memzero(my_ephem_sk, sizeof(my_ephem_sk));
        sodium_memzero(my_ephem_pk, sizeof(my_ephem_pk));
    }

    SessionState()                               = default;
    SessionState(const SessionState&)            = delete;
    SessionState& operator=(const SessionState&) = delete;
    SessionState(SessionState&&)                 = delete;

    ~SessionState() {
        sodium_memzero(session_key, sizeof(session_key));
        sodium_memzero(my_ephem_sk, sizeof(my_ephem_sk));
    }
};

// ── ConnectionRecord ──────────────────────────────────────────────────────────

/// Full state of one peer connection.
struct ConnectionRecord {
    conn_id_t    id;
    conn_state_t state            = STATE_AUTH_PENDING;
    endpoint_t   remote;
    std::string  local_scheme;

    std::vector<std::string> peer_schemes;
    std::string              negotiated_scheme;

    uint8_t peer_user_pubkey  [32]{};
    uint8_t peer_device_pubkey[32]{};
    bool    peer_authenticated = false;
    bool    is_localhost       = false;

    msg::CoreMeta peer_core_meta{};

    /// Plugin name pinned on first PROPAGATION_CONSUMED result.
    std::string affinity_plugin;

    std::atomic<uint64_t> send_packet_id{0};

    std::unique_ptr<SessionState> session;
    std::vector<uint8_t>          recv_buf;
};

// ── HandlerEntry ──────────────────────────────────────────────────────────────

/// Registered handler slot inside ConnectionManager.
struct HandlerEntry {
    std::string           name;
    handler_t*            handler  = nullptr;
    uint8_t               priority = 128;
    std::vector<uint32_t> subscribed_types;
};

// ── Subscription ─────────────────────────────────────────────────────────────

/// RAII unsubscription token returned by subscribe() calls.
class Subscription {
public:
    Subscription() = default;
    Subscription(uint64_t id, std::function<void(uint64_t)> fn)
        : id_(id), fn_(std::move(fn)) {}

    ~Subscription() { if (fn_ && id_) fn_(id_); }

    Subscription(const Subscription&)            = delete;
    Subscription& operator=(const Subscription&) = delete;
    Subscription(Subscription&&) noexcept        = default;
    Subscription& operator=(Subscription&&) noexcept = default;

    [[nodiscard]] uint64_t id() const noexcept { return id_; }

private:
    uint64_t                      id_ = 0;
    std::function<void(uint64_t)> fn_;
};

} // namespace gn