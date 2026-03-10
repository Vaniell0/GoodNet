#pragma once

/// @file core/connectionManager.hpp
/// @brief Peer connection lifecycle, handshake, encryption, and packet dispatch.
///
/// Handshake (simplified Noise_XX):
///   1. AUTH — both peers send immediately after connect:
///      user_pubkey[32] | device_pubkey[32] | Ed25519sig[64] | ephem_pubkey[32] | schemes[]
///      Signature covers user_pk || device_pk || ephem_pk — prevents replay.
///   2. ECDH (local):
///      shared     = X25519(my_ephem_sk, peer_ephem_pk)
///      session_key = BLAKE2b-256(shared || min(pk_a, pk_b) || max(pk_a, pk_b))
///   3. ESTABLISHED: payload encrypted via XSalsa20-Poly1305, header signed with device_key.
///      Localhost (127.x/::1): AUTH runs for identification, crypto skipped.
///
/// File layout:
///   cm_identity.cpp  — NodeIdentity, SSH key parser
///   cm_session.cpp   — SessionState encrypt/decrypt/derive
///   cm_handshake.cpp — connect/auth/disconnect lifecycle
///   cm_dispatch.cpp  — TCP reassembly, packet dispatch
///   cm_send.cpp      — send, send_frame, scheme negotiation

#include <memory>
#include <string>
#include <vector>
#include <optional>
#include <filesystem>

#include <sodium/crypto_secretbox.h> // для crypto_secretbox_KEYBYTES
#include <sodium/crypto_sign.h>      // для crypto_sign_PUBLICKEYBYTES и т.д.
#include <sodium/crypto_box.h>       // для crypto_box_PUBLICKEYBYTES и т.д.
#include <sodium/utils.h>            // для sodium_memzero

#include "logger.hpp"
#include "signals.hpp"
#include "data/machine_id.hpp"

#include "../sdk/types.h"
#include "../sdk/handler.h"
#include "../sdk/connector.h"

namespace gn {

namespace fs = std::filesystem;

/// @brief Identity loading configuration (maps to config.json "identity" section).
struct IdentityConfig {
    fs::path dir            = "~/.goodnet";
    fs::path ssh_key_path   = ""; ///< Empty = auto-detect ~/.ssh/id_ed25519
    bool     use_machine_id = true;
};

/// @brief Node cryptographic identity (user + device keypairs).
struct NodeIdentity {
    uint8_t user_pubkey  [crypto_sign_PUBLICKEYBYTES]{};
    uint8_t user_seckey  [crypto_sign_SECRETKEYBYTES]{};
    uint8_t device_pubkey[crypto_sign_PUBLICKEYBYTES]{};
    uint8_t device_seckey[crypto_sign_SECRETKEYBYTES]{};

    static NodeIdentity load_or_generate(const IdentityConfig& cfg);

    /// @brief Convenience overload for tests.
    static NodeIdentity load_or_generate(const fs::path& dir) {
        return load_or_generate(IdentityConfig{ .dir = dir });
    }

    std::string user_pubkey_hex()   const;
    std::string device_pubkey_hex() const;

    /// @brief Try to load unencrypted OpenSSH Ed25519 private key.
    static bool try_load_ssh_key(const fs::path& path,
                                  uint8_t out_pub[crypto_sign_PUBLICKEYBYTES],
                                  uint8_t out_sec[crypto_sign_SECRETKEYBYTES]);
private:
    static void load_or_gen_keypair(const fs::path& path,
                                     uint8_t out_pub[crypto_sign_PUBLICKEYBYTES],
                                     uint8_t out_sec[crypto_sign_SECRETKEYBYTES]);
    static void save_key(const fs::path& path, const uint8_t* key, size_t size);
};

/// @brief Wire format for MSG_TYPE_AUTH.
/// @details kBaseSize (160) = pubkeys(64) + sig(64) + ephem_pk(32)
///          kFullSize (289) = kBaseSize + schemes_count(1) + schemes[8][16]
///          Old clients send payload_len == kBaseSize → schemes_count treated as 0.
static constexpr uint8_t AUTH_MAX_SCHEMES = 8;
static constexpr uint8_t AUTH_SCHEME_LEN  = 16;

#pragma pack(push, 1)
struct auth_payload_t {
    uint8_t user_pubkey  [32];
    uint8_t device_pubkey[32];
    uint8_t signature    [64];
    uint8_t ephem_pubkey [32];

    uint8_t schemes_count;
    char    schemes[AUTH_MAX_SCHEMES][AUTH_SCHEME_LEN];

    static constexpr size_t kBaseSize = 32 + 32 + 64 + 32;
    static constexpr size_t kFullSize = kBaseSize + 1 + AUTH_MAX_SCHEMES * AUTH_SCHEME_LEN;

    void set_schemes(const std::vector<std::string>& sv) {
        schemes_count = static_cast<uint8_t>(
            std::min(sv.size(), static_cast<size_t>(AUTH_MAX_SCHEMES)));
        for (uint8_t i = 0; i < schemes_count; ++i) {
            std::strncpy(schemes[i], sv[i].c_str(), AUTH_SCHEME_LEN - 1);
            schemes[i][AUTH_SCHEME_LEN - 1] = '\0';
        }
        for (uint8_t i = schemes_count; i < AUTH_MAX_SCHEMES; ++i)
            schemes[i][0] = '\0';
    }

    std::vector<std::string> get_schemes() const {
        std::vector<std::string> out; out.reserve(schemes_count);
        for (uint8_t i = 0; i < schemes_count; ++i) out.emplace_back(schemes[i]);
        return out;
    }
};
#pragma pack(pop)

static_assert(sizeof(auth_payload_t) == auth_payload_t::kFullSize,
              "auth_payload_t size mismatch — check padding");

/// @brief Per-connection cryptographic session state (post-ECDH).
/// @details session_key = BLAKE2b-256(X25519(ephem) || min(pk_a,pk_b) || max(pk_a,pk_b))
///          Wire: nonce_u64_le(8) + XSalsa20-Poly1305(plain).
///          Replay protection: recv_nonce_expected is strictly monotonic.
struct SessionState {
    uint8_t session_key[crypto_secretbox_KEYBYTES]{};

    bool ready = false;

    uint8_t my_ephem_pk[crypto_box_PUBLICKEYBYTES]{};
    uint8_t my_ephem_sk[crypto_box_SECRETKEYBYTES]{};

    std::atomic<uint64_t> send_nonce{1};
    std::atomic<uint64_t> recv_nonce_expected{1};

    std::vector<uint8_t> encrypt(const void* plain, size_t plain_len);
    std::vector<uint8_t> decrypt(const void* wire,  size_t wire_len);

    void clear_ephemeral() {
        sodium_memzero(my_ephem_sk, sizeof(my_ephem_sk));
        sodium_memzero(my_ephem_pk, sizeof(my_ephem_pk));
    }

    ~SessionState() {
        sodium_memzero(session_key, sizeof(session_key));
        sodium_memzero(my_ephem_sk, sizeof(my_ephem_sk));
    }

    SessionState()                               = default;
    SessionState(const SessionState&)            = delete;
    SessionState& operator=(const SessionState&) = delete;
    SessionState(SessionState&&)                 = delete;
};

/// @brief Per-connection mutable state tracked by ConnectionManager.
struct ConnectionRecord {
    conn_id_t    id;
    conn_state_t state       = STATE_AUTH_PENDING;
    endpoint_t   remote;
    std::string  local_scheme;

    std::vector<std::string> peer_schemes;
    std::string  negotiated_scheme;

    uint8_t peer_user_pubkey  [32]{};
    uint8_t peer_device_pubkey[32]{};
    bool    peer_authenticated = false;
    bool    is_localhost       = false;

    std::unique_ptr<SessionState> session;
    std::vector<uint8_t>          recv_buf;
};

/// @brief Registered handler with metadata.
struct HandlerEntry {
    std::string           name;
    handler_t*            handler = nullptr;
    std::vector<uint32_t> subscribed_types;
};

/// @brief Central connection manager: handshake, crypto, routing, plugin API.
class ConnectionManager {
public:
    explicit ConnectionManager(SignalBus& bus, NodeIdentity identity);
    ~ConnectionManager();

    ConnectionManager(const ConnectionManager&)            = delete;
    ConnectionManager& operator=(const ConnectionManager&) = delete;

    void register_connector(const std::string& scheme, connector_ops_t* ops);
    void register_handler  (handler_t* h);

    /// @brief Set transport scheme preference order for negotiate_scheme().
    void set_scheme_priority(std::vector<std::string> p);

    /// @brief Fill host_api_t callbacks for plugin initialization.
    void fill_host_api(host_api_t* api);

    /// @brief Send packet to uri. Connects if no active session exists.
    void send(const char* uri, uint32_t msg_type, const void* payload, size_t size);

    void shutdown();

    size_t                      connection_count()       const;
    std::vector<std::string>    get_active_uris()        const;
    std::optional<conn_state_t> get_state(conn_id_t id)  const;
    const std::atomic<size_t>& get_pending_bytes() const { return pending_bytes_; }
    std::optional<std::string>  get_negotiated_scheme(conn_id_t id) const;
    const NodeIdentity&         identity()               const { return identity_; }

private:
    conn_id_t handle_connect   (const endpoint_t* ep);
    void      handle_disconnect(conn_id_t id, int error);
    void      send_auth        (conn_id_t id);
    bool      process_auth     (conn_id_t id, const uint8_t* payload, size_t size);
    
    // ─── MEMORY GUARD + CHUNKING ─────────────────────────────────────
    std::atomic<size_t> pending_bytes_{0};
    static constexpr size_t MAX_IN_FLIGHT_BYTES = 512UL * 1024 * 1024;
    static constexpr size_t CHUNK_SIZE = 1UL * 1024 * 1024;

    bool derive_session(conn_id_t id,
                        const uint8_t peer_ephem_pk[32],
                        const uint8_t peer_user_pk[32]);

    void handle_data    (conn_id_t id, const void* raw, size_t size);
    void dispatch_packet(conn_id_t id, const header_t* hdr,
                         const uint8_t* payload, size_t payload_size);

    void             send_frame      (conn_id_t id, uint32_t msg_type,
                                      const void* payload, size_t size);
    std::string      negotiate_scheme(const ConnectionRecord& rec) const;
    std::vector<std::string> local_schemes() const;
    std::optional<conn_id_t> resolve_uri(const std::string& uri);
    connector_ops_t* find_connector  (const std::string& scheme);
    static bool      is_localhost_address(std::string_view address);

    static conn_id_t s_on_connect   (void*, const endpoint_t*);
    static void      s_on_data      (void*, conn_id_t, const void*, size_t);
    static void      s_on_disconnect(void*, conn_id_t, int);
    static void      s_send         (void*, const char*, uint32_t, const void*, size_t);
    static int       s_sign         (void*, const void*, size_t, uint8_t[64]);
    static int       s_verify       (void*, const void*, size_t, const uint8_t*, const uint8_t*);

    SignalBus&    bus_;
    NodeIdentity  identity_;
    std::atomic<bool> shutting_down_{false};

    std::vector<std::string> scheme_priority_ = {"tcp", "ws", "udp", "mock"};

    mutable std::shared_mutex handlers_mu_;
    std::unordered_map<std::string, HandlerEntry> handler_entries_;

    mutable std::shared_mutex records_mu_;
    std::unordered_map<conn_id_t, ConnectionRecord> records_;
    std::atomic<conn_id_t> next_id_{1};

    mutable std::shared_mutex connectors_mu_;
    std::unordered_map<std::string, connector_ops_t*> connectors_;

    mutable std::shared_mutex uri_mu_;
    std::unordered_map<std::string, conn_id_t> uri_index_;

    mutable std::shared_mutex pk_mu_;
    std::unordered_map<std::string, conn_id_t> pk_index_;
};

} // namespace gn
