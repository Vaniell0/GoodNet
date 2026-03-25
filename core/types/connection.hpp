#pragma once
/// @file core/types/connection.hpp
/// @brief Connection state types: NoiseSession, ConnectionRecord, HandlerEntry.

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <functional>

#include <sodium/crypto_sign.h>
#include <sodium/utils.h>

#include "nonce_window.hpp"
#include "crypto/noise.hpp"
#include "data/messages.hpp"
#include "../sdk/handler.h"

struct connector_ops_t; // forward (sdk/connector.h)

namespace gn {

// ── TransportPath ────────────────────────────────────────────────────────────

/// @brief Один транспортный путь к пиру.
/// ConnectionRecord::transport_paths содержит все доступные пути.
/// Ядро выбирает лучший активный путь для отправки.
///
/// connector_ops НЕ кэшируется — разрешается динамически через find_connector(scheme).
/// Причина: коннекторы могут перерегистрироваться, и кэшированный указатель может стать dangling.
struct TransportPath {
    conn_id_t   transport_conn_id = CONN_ID_INVALID; ///< ID, известный коннектору
    std::string scheme;                              ///< "tcp", "ice", "ws"...
    uint8_t     priority = 255;                      ///< 0 = наивысший, из scheme_priority_
    endpoint_t  remote{};                            ///< Адрес пира на этом транспорте
    bool        active = true;                       ///< Путь работает
    uint64_t    last_rtt_us = 0;                     ///< Последний RTT (микросекунды)
    uint32_t    consecutive_errors = 0;              ///< Ошибок подряд; 3+ → active=false
    std::chrono::steady_clock::time_point added_at{};///< Время добавления
};

// ── NoiseSession ─────────────────────────────────────────────────────────────

/// @brief Per-connection transport encryption state after Noise_XX handshake.
///
/// Created by `finalize_handshake()` after msg3 completes.
/// Uses ChaChaPoly-IETF AEAD with `packet_id` as nonce (from header_t).
///
/// Thread-safety: encrypt() and decrypt() must not be called concurrently
/// on the same session — the core serializes per-connection operations.
///
/// Ownership: held by `ConnectionRecord::session` as unique_ptr.
/// Destroyed when the connection closes — keys are securely wiped.
struct NoiseSession {
    uint8_t send_key[noise::KEYLEN]{};        ///< AEAD send key (ChaChaPoly-IETF)
    uint8_t recv_key[noise::KEYLEN]{};        ///< AEAD receive key (ChaChaPoly-IETF)
    uint8_t handshake_hash[noise::HASHLEN]{}; ///< Channel binding token (h after split)
    NonceWindow recv_window;                  ///< Anti-replay window for inbound packets

    /// @brief Encrypt payload with optional zstd compression.
    /// @param plain              Plaintext payload.
    /// @param len                Plaintext byte count.
    /// @param nonce              packet_id from header (used as AEAD nonce).
    /// @param compress_enabled   Enable zstd compression attempt.
    /// @param compress_threshold Minimum payload size to trigger compression.
    /// @param compress_level     Zstd compression level.
    /// @return AEAD ciphertext (may include zstd prefix if compressed).
    std::vector<uint8_t> encrypt(const void* plain, size_t len,
                                  uint64_t nonce,
                                  bool compress_enabled,
                                  int compress_threshold,
                                  int compress_level);

    /// @brief Decrypt AEAD ciphertext and decompress if zstd-prefixed.
    /// @param wire   Wire bytes (ciphertext).
    /// @param len    Wire byte count.
    /// @param nonce  packet_id from header (used as AEAD nonce).
    /// @return Decrypted plaintext, or empty vector on failure.
    std::vector<uint8_t> decrypt(const void* wire, size_t len,
                                  uint64_t nonce);

    NoiseSession()                             = default;
    NoiseSession(const NoiseSession&)          = delete;
    NoiseSession& operator=(const NoiseSession&) = delete;

    ~NoiseSession() {
        sodium_memzero(send_key, sizeof(send_key));
        sodium_memzero(recv_key, sizeof(recv_key));
        sodium_memzero(handshake_hash, sizeof(handshake_hash));
    }
};

// ── ConnectionRecord ─────────────────────────────────────────────────────────

/// @brief Full state of one peer connection.
///
/// Lifecycle: allocated on on_connect(), lives in the RCU RecordMap,
/// destroyed after on_disconnect() + RCU grace period.
///
/// Thread-safety: fields are partitioned into immutable-after-creation
/// (id, remote, local_scheme, is_initiator) and mutable-under-lock
/// (state, session, peer_*, heartbeat atomics).
struct ConnectionRecord {
    conn_id_t    id;                              ///< Unique connection ID (immutable)
    conn_state_t state = STATE_NOISE_HANDSHAKE;   ///< Current lifecycle state
    endpoint_t   remote;                          ///< Peer address/port/flags (set on connect)
    std::string  local_scheme;                    ///< Connector scheme used ("tcp", "ice", etc.)

    std::vector<std::string> peer_schemes;        ///< Schemes advertised by peer in handshake
    std::string              negotiated_scheme;   ///< Best common scheme after negotiation

    uint8_t peer_user_pubkey  [crypto_sign_PUBLICKEYBYTES]{}; ///< Peer Ed25519 user pubkey (valid after handshake)
    uint8_t peer_device_pubkey[crypto_sign_PUBLICKEYBYTES]{}; ///< Peer Ed25519 device pubkey
    bool    peer_authenticated  = false;          ///< true after signature + Noise key verification
    bool    is_localhost        = false;           ///< true if connector set EP_FLAG_TRUSTED
    bool    localhost_passthrough = false;         ///< true = skip AEAD for this connection
    bool    is_initiator        = false;           ///< true = outgoing (sends NOISE_INIT)

    msg::CoreMeta peer_core_meta{};               ///< Peer capabilities from handshake

    /// @brief Статус транспортных путей пира (из heartbeat extension).
    struct PeerPathInfo {
        std::string scheme;
        bool        active   = true;
        uint8_t     priority = 255;
        uint16_t    rtt_compressed = 0; ///< RTT in 10us units
    };
    std::vector<PeerPathInfo> peer_transport_info; ///< Updated from heartbeat

    /// @brief Handler pinned by PROPAGATION_CONSUMED (session affinity).
    std::string affinity_plugin;

    std::atomic<uint64_t> send_packet_id{0};      ///< Monotonic AEAD nonce counter

    // Heartbeat keepalive state
    std::atomic<int64_t>  last_heartbeat_recv{0}; ///< Timestamp of last heartbeat (microseconds)
    std::atomic<uint32_t> heartbeat_seq{0};        ///< Monotonic heartbeat sequence counter
    std::atomic<uint32_t> missed_heartbeats{0};    ///< Consecutive missed heartbeats (3 = disconnect)

    /// @brief Noise_XX handshake state.
    ///        Active during handshake, reset to nullptr after split().
    std::unique_ptr<noise::HandshakeState> handshake;

    /// @brief Transport session — AEAD keys + anti-replay.
    ///        Active after ESTABLISHED, nullptr before.
    std::unique_ptr<NoiseSession> session;

    /// @brief Receive buffer for TCP stream reassembly.
    std::vector<uint8_t> recv_buf;

    // ── Multi-transport ──────────────────────────────────────────────────────

    /// @brief Все транспортные пути к этому пиру.
    /// Заполняется в handle_connect (первичный путь), расширяется при merge.
    std::vector<TransportPath> transport_paths;

    /// @brief Лучший активный путь (наименьший priority среди active).
    TransportPath* best_path() {
        TransportPath* best = nullptr;
        for (auto& p : transport_paths)
            if (p.active && (!best || p.priority < best->priority))
                best = &p;
        return best;
    }
    const TransportPath* best_path() const {
        const TransportPath* best = nullptr;
        for (auto& p : transport_paths)
            if (p.active && (!best || p.priority < best->priority))
                best = &p;
        return best;
    }

    /// @brief Найти путь по scheme.
    TransportPath* find_path(std::string_view s) {
        for (auto& p : transport_paths)
            if (p.scheme == s) return &p;
        return nullptr;
    }

    /// @brief Найти путь по transport_conn_id.
    TransportPath* find_path_by_transport_id(conn_id_t tcid) {
        for (auto& p : transport_paths)
            if (p.transport_conn_id == tcid) return &p;
        return nullptr;
    }
};

// ── HandlerEntry ─────────────────────────────────────────────────────────────

/// @brief Источник регистрации хэндлера — определяет уровень доверия.
enum class HandlerSource : uint8_t {
    Plugin,      ///< Загружен как standalone handler plugin
    Connector,   ///< Зарегистрирован коннектором через host_api->register_handler
    Internal,    ///< Внутренний handler ядра
};

/// @brief Registered handler metadata (used by PluginManager + dispatch).
struct HandlerEntry {
    std::string           name;                ///< Handler name (plugin_info->name)
    handler_t*            handler  = nullptr;  ///< C handler descriptor (plugin-owned)
    uint8_t               priority = 128;      ///< Dispatch priority (0=highest)
    std::vector<uint32_t> subscribed_types;    ///< MSG_TYPE_* subscriptions (empty=wildcard)
    HandlerSource         source = HandlerSource::Plugin; ///< Кто зарегистрировал
};

// ── Subscription ─────────────────────────────────────────────────────────────

/// @brief RAII subscription handle — unsubscribes on destruction.
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
