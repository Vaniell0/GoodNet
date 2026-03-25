#pragma once
/// @file core/cm/connectionManager.hpp
/// @brief ConnectionManager — owns all peer connections, Noise handshakes, and transport.
///
/// ## Responsibilities
///   - Connector/handler registration and lifecycle
///   - Noise_XX handshake orchestration (3-message: init → resp → fin)
///   - AEAD encryption/decryption (ChaChaPoly-IETF, packet_id as nonce)
///   - Frame building/parsing (header_t + payload)
///   - Gossip relay with O(1) dedup
///   - Heartbeat (30s interval, 3 missed → disconnect)
///   - Pending message queue for pre-ESTABLISHED sends
///
/// ## Thread-safety
/// All public methods are thread-safe. The connection registry uses RCU
/// (read-copy-update) for lock-free reads on the hot path. Writes (connect,
/// disconnect) take a mutex to rebuild the map atomically.
///
/// ## Implementation
/// Details hidden behind Pimpl — see `core/cm_impl.hpp` for the Impl struct.
/// Implementation split across: cm_lifecycle.cpp, cm_transport.cpp,
/// cm_handshake.cpp, cm_dispatch.cpp, cm_relay.cpp, cm_identity.cpp.

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "types/identify.hpp"
#include "data/messages.hpp"

#include "../sdk/connector.h"
#include "../sdk/handler.h"

class CMTest;
class Config;
class HeartbeatTest;

namespace gn {

class SignalBus;

class ConnectionManager {
public:
    /// @param bus      SignalBus for packet dispatch and stats.
    /// @param identity Node identity (user + device Ed25519 keypairs).
    /// @param config   Config pointer (borrowed). nullptr = internal defaults.
    explicit ConnectionManager(SignalBus& bus, NodeIdentity identity,
                               Config* config = nullptr);
    ~ConnectionManager();

    ConnectionManager(const ConnectionManager&)            = delete;
    ConnectionManager& operator=(const ConnectionManager&) = delete;

    /// @name Plugin registration
    /// @{

    /// @brief Register a message handler plugin.
    void register_handler    (handler_t* h);

    /// @brief Register a transport connector for a URI scheme (e.g. "tcp").
    void register_connector  (const std::string& scheme, connector_ops_t* ops);

    /// @brief Set preferred scheme order for auto-connect (first = highest priority).
    void set_scheme_priority (std::vector<std::string> priority);

    /// @brief Fill a host_api_t vtable with all CM callbacks for plugin use.
    void fill_host_api(host_api_t* api);
    /// @}

    /// @name Send / Broadcast
    /// @{

    /// @brief Send to a peer by URI. Auto-connects if needed, queues if not ESTABLISHED.
    /// @return false on backpressure or invalid URI.
    bool send(std::string_view uri, uint32_t msg_type,
              std::span<const uint8_t> payload);

    /// @brief Send on an existing connection by ID.
    /// @return false if connection not found or not ESTABLISHED.
    bool send(conn_id_t id, uint32_t msg_type,
              std::span<const uint8_t> payload);

    /// @brief Broadcast to all ESTABLISHED peers.
    void broadcast(uint32_t msg_type, std::span<const uint8_t> payload);
    /// @}

    /// @name Connection control
    /// @{
    void connect(std::string_view uri);   ///< Initiate outbound connection.
    void disconnect(conn_id_t id);        ///< Graceful close (drain queue).
    void close_now(conn_id_t id);         ///< Hard close (immediate).
    void shutdown();                      ///< Close all connections, wait for in-flight ops.
    /// @}

    /// @name Key management
    /// @{

    /// @brief Rotate long-term identity keys (PFS — existing sessions unaffected).
    void rotate_identity_keys(const Config::Identity& cfg);

    /// @brief Re-derive transport keys for an ESTABLISHED session (Noise rekey via HKDF).
    bool rekey_session(conn_id_t id);
    /// @}

    /// @name Gossip relay
    /// @{

    /// @brief Forward a frame to all peers except `exclude_conn`, decrementing TTL.
    void relay(conn_id_t exclude_conn, uint8_t ttl,
               const uint8_t dest_pubkey[GN_SIGN_PUBLICKEYBYTES],
               std::span<const uint8_t> inner_frame);
    /// @}

    /// @name Heartbeat / maintenance
    /// @{
    void check_heartbeat_timeouts();  ///< Disconnect peers with 3+ missed heartbeats.
    void cleanup_stale_pending();     ///< Drop pending messages older than handshake timeout.
    /// @}

    /// @name Queries
    /// @{
    [[nodiscard]] size_t                      connection_count()              const;
    [[nodiscard]] std::vector<std::string>    get_active_uris()               const;
    [[nodiscard]] std::vector<conn_id_t>      get_active_conn_ids()           const;
    [[nodiscard]] std::optional<conn_state_t> get_state(conn_id_t id)         const;
    [[nodiscard]] std::optional<std::string>  get_negotiated_scheme(conn_id_t id) const;
    [[nodiscard]] std::optional<std::vector<uint8_t>> get_peer_pubkey(conn_id_t id) const;
    [[nodiscard]] std::string get_peer_pubkey_hex(conn_id_t id) const;
    [[nodiscard]] std::optional<endpoint_t> get_peer_endpoint(conn_id_t id) const;
    [[nodiscard]] conn_id_t find_conn_by_pubkey(const char* pubkey_hex)        const;
    [[nodiscard]] size_t get_pending_bytes(conn_id_t id = CONN_ID_INVALID)    const noexcept;

    /// @brief JSON diagnostic dump of all active connections (id, state, peer, scheme).
    [[nodiscard]] std::string dump_connections() const;

    [[nodiscard]] const NodeIdentity& identity() const;
    msg::CoreMeta local_core_meta() const;
    /// @}

    struct Impl;

private:
    friend class ::CMTest;
    friend class ::HeartbeatTest;
    std::unique_ptr<Impl> impl_;
};

} // namespace gn