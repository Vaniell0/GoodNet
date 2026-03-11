#pragma once

/// @file core/connectionManager.hpp
/// @brief Peer connection lifecycle, handshake, encryption, and packet dispatch.

#include <memory>
#include <string>
#include <vector>
#include <optional>
#include <atomic>

#include <sodium/crypto_secretbox.h>
#include <sodium/crypto_sign.h>
#include <sodium/crypto_box.h>
#include <sodium/utils.h>

#include "logger.hpp"
#include "signals.hpp"
#include "data/machine_id.hpp"
#include "data/messages.hpp"

#include "../sdk/handler.h"
#include "../sdk/connector.h"

namespace gn {

namespace fs = std::filesystem;

// ─── core_meta_t alias ───────────────────────────────────────────────────────
// CoreMeta lives in sdk/messages.hpp (application-layer payload data).
// This alias makes the existing connectionManager code compile without changes.
using core_meta_t = gn::msg::CoreMeta;

// ─── Identity configuration ───────────────────────────────────────────────────

struct IdentityConfig {
    fs::path dir            = "~/.goodnet";
    fs::path ssh_key_path   = "";
    bool     use_machine_id = true;
};

// ─── NodeIdentity ─────────────────────────────────────────────────────────────

struct NodeIdentity {
    uint8_t user_pubkey  [crypto_sign_PUBLICKEYBYTES]{};
    uint8_t user_seckey  [crypto_sign_SECRETKEYBYTES]{};
    uint8_t device_pubkey[crypto_sign_PUBLICKEYBYTES]{};
    uint8_t device_seckey[crypto_sign_SECRETKEYBYTES]{};

    static NodeIdentity load_or_generate(const IdentityConfig& cfg);
    static NodeIdentity load_or_generate(const fs::path& dir) {
        return load_or_generate(IdentityConfig{ .dir = dir });
    }

    std::string user_pubkey_hex()   const;
    std::string device_pubkey_hex() const;

    static bool try_load_ssh_key(const fs::path& path,
                                  uint8_t out_pub[crypto_sign_PUBLICKEYBYTES],
                                  uint8_t out_sec[crypto_sign_SECRETKEYBYTES]);
private:
    static void load_or_gen_keypair(const fs::path& path,
                                     uint8_t out_pub[crypto_sign_PUBLICKEYBYTES],
                                     uint8_t out_sec[crypto_sign_SECRETKEYBYTES]);
    static void save_key(const fs::path& path, const uint8_t* key, size_t size);
};

// ─── AUTH wire payload ────────────────────────────────────────────────────────

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

    core_meta_t core_meta;   // gn::msg::CoreMeta via alias above

    static constexpr size_t kBaseSize    = 32 + 32 + 64 + 32;
    static constexpr size_t kSchemeBlock = 1 + AUTH_MAX_SCHEMES * AUTH_SCHEME_LEN;
    static constexpr size_t kFullSize    = kBaseSize + kSchemeBlock + sizeof(core_meta_t);

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
              "auth_payload_t size mismatch — check padding / core_meta_t size");

// ─── StatsCollector ───────────────────────────────────────────────────────────
// Lock-free hot-path counters.  Sampled by StatsEmitter / CLI stats command.

struct StatsCollector {
    std::atomic<uint64_t> rx_bytes    {0};
    std::atomic<uint64_t> tx_bytes    {0};
    std::atomic<uint64_t> rx_packets  {0};
    std::atomic<uint64_t> tx_packets  {0};

    std::atomic<uint64_t> auth_ok     {0};
    std::atomic<uint64_t> auth_fail   {0};
    std::atomic<uint64_t> decrypt_fail{0};
    std::atomic<uint64_t> dropped_bp  {0};  ///< Backpressure drops

    std::atomic<uint64_t> consumed    {0};  ///< PROPAGATION_CONSUMED count
    std::atomic<uint64_t> rejected    {0};  ///< PROPAGATION_REJECT count

    std::atomic<uint32_t> connections {0};
    std::atomic<uint32_t> total_conn  {0};
    std::atomic<uint32_t> total_disc  {0};

    void on_rx(size_t bytes) noexcept {
        rx_bytes  .fetch_add(bytes, std::memory_order_relaxed);
        rx_packets.fetch_add(1,     std::memory_order_relaxed);
    }
    void on_tx(size_t bytes) noexcept {
        tx_bytes  .fetch_add(bytes, std::memory_order_relaxed);
        tx_packets.fetch_add(1,     std::memory_order_relaxed);
    }
    void on_connect()    noexcept {
        connections.fetch_add(1, std::memory_order_relaxed);
        total_conn .fetch_add(1, std::memory_order_relaxed);
    }
    void on_disconnect() noexcept {
        connections.fetch_sub(1, std::memory_order_relaxed);
        total_disc .fetch_add(1, std::memory_order_relaxed);
    }
};

// ─── SessionState ─────────────────────────────────────────────────────────────

struct SessionState {
    uint8_t session_key[crypto_secretbox_KEYBYTES]{};
    bool    ready = false;

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

// ─── ConnectionRecord ─────────────────────────────────────────────────────────

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

    core_meta_t peer_core_meta{};   ///< Capabilities from remote AUTH

    std::string affinity_plugin;    ///< Plugin name pinned on first CONSUMED

    std::unique_ptr<SessionState> session;
    std::vector<uint8_t>          recv_buf;
};

// ─── HandlerEntry ─────────────────────────────────────────────────────────────

struct HandlerEntry {
    std::string           name;
    handler_t*            handler  = nullptr;
    uint8_t               priority = 128;
    std::vector<uint32_t> subscribed_types;
};

// ─── ConnectionManager ────────────────────────────────────────────────────────

class ConnectionManager {
public:
    explicit ConnectionManager(SignalBus& bus, NodeIdentity identity);
    ~ConnectionManager();

    ConnectionManager(const ConnectionManager&)            = delete;
    ConnectionManager& operator=(const ConnectionManager&) = delete;

    void register_connector(const std::string& scheme, connector_ops_t* ops);
    void register_handler  (handler_t* h);

    void set_scheme_priority(std::vector<std::string> p);
    void fill_host_api(host_api_t* api);

    void send(const char* uri, uint32_t msg_type, const void* payload, size_t size);
    void send_on_conn(conn_id_t id, uint32_t msg_type,
                      const void* payload, size_t size);

    void rotate_identity_keys(const IdentityConfig& cfg);
    static core_meta_t local_core_meta();
    conn_id_t find_conn_by_pubkey(const char* pubkey_hex) const;

    void shutdown();

    size_t                      connection_count()       const;
    std::vector<std::string>    get_active_uris()        const;
    std::optional<conn_state_t> get_state(conn_id_t id)  const;
    std::optional<std::string>  get_negotiated_scheme(conn_id_t id) const;

    size_t get_pending_bytes() const {
        return pending_bytes_.load(std::memory_order_relaxed);
    }

    const NodeIdentity&   identity() const { return identity_; }
    const StatsCollector& stats()    const { return stats_; }
    StatsCollector&       stats()          { return stats_; }

private:
    conn_id_t handle_connect   (const endpoint_t* ep);
    void      handle_disconnect(conn_id_t id, int error);
    void      handle_data      (conn_id_t id, const void* raw, size_t size);
    void      dispatch_packet  (conn_id_t id, const header_t* hdr,
                                 const uint8_t* payload, size_t payload_size);

    void send_auth    (conn_id_t id);
    bool process_auth (conn_id_t id, const uint8_t* payload, size_t size);
    bool derive_session(conn_id_t id,
                        const uint8_t peer_ephem_pk[32],
                        const uint8_t peer_user_pk[32]);

    void             send_frame      (conn_id_t id, uint32_t msg_type,
                                      const void* payload, size_t size);
    std::string      negotiate_scheme(const ConnectionRecord& rec) const;
    std::vector<std::string> local_schemes() const;
    std::optional<conn_id_t> resolve_uri(const std::string& uri);
    connector_ops_t* find_connector  (const std::string& scheme);
    static bool      is_localhost_address(std::string_view address);

    std::atomic<size_t> pending_bytes_{0};
    static constexpr size_t MAX_IN_FLIGHT_BYTES = 512UL * 1024 * 1024;
    static constexpr size_t CHUNK_SIZE          = 1UL  * 1024 * 1024;

    static conn_id_t s_on_connect      (void*, const endpoint_t*);
    static void      s_on_data         (void*, conn_id_t, const void*, size_t);
    static void      s_on_disconnect   (void*, conn_id_t, int);
    static void      s_send            (void*, const char*, uint32_t,
                                        const void*, size_t);
    static void      s_send_response   (void*, conn_id_t, uint32_t,
                                        const void*, size_t);
    static int       s_sign            (void*, const void*, size_t, uint8_t[64]);
    static int       s_verify          (void*, const void*, size_t,
                                        const uint8_t*, const uint8_t*);
    static conn_id_t s_find_conn_by_pk (void*, const char*);
    static void      s_register_handler(void*, handler_t*);
    static void      s_log             (void*, int, const char*, int, const char*);

    SignalBus&    bus_;

    mutable std::shared_mutex identity_mu_;
    NodeIdentity  identity_;

    StatsCollector stats_;          ///< Lock-free performance counters

    std::atomic<bool> shutting_down_{false};

    std::vector<std::string> scheme_priority_ = {"tcp", "ws", "udp", "mock", "ice"};

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