#pragma once
/// @file core/cm_impl.hpp
/// Приватная реализация ConnectionManager (Pimpl).
/// Включается только из core/cm_*.cpp и тестов, которым нужен доступ к internals.

#include "connectionManager.hpp"
#include "signals.hpp"
#include "types/connection.hpp"
#include "types/pending.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gn {

// ── PerConnQueue ──────────────────────────────────────────────────────────────

/// Per-connection outbound frame queue with independent backpressure limit.
struct PerConnQueue {
    static constexpr size_t MAX_BYTES = 8 * 1024 * 1024;  // 8 MB per-conn

    std::mutex               mu;
    std::vector<std::vector<uint8_t>> frames;
    std::atomic<size_t>      pending_bytes{0};
    std::atomic<bool>        draining{false};

    /// @brief Atomically reserve space and enqueue a frame.
    /// @return false if backpressure limit would be exceeded.
    bool try_push(std::vector<uint8_t> frame) {
        const size_t sz = frame.size();
        const size_t prev = pending_bytes.fetch_add(sz, std::memory_order_relaxed);
        if (prev + sz > MAX_BYTES) {
            pending_bytes.fetch_sub(sz, std::memory_order_relaxed);
            return false;
        }
        std::lock_guard lock(mu);
        frames.push_back(std::move(frame));
        return true;
    }

    /// @brief Dequeue up to @p max_frames frames, decrementing pending_bytes.
    std::vector<std::vector<uint8_t>> drain_batch(size_t max_frames = 64) {
        std::lock_guard lock(mu);
        size_t n = std::min(frames.size(), max_frames);
        std::vector<std::vector<uint8_t>> batch(
            std::make_move_iterator(frames.begin()),
            std::make_move_iterator(frames.begin() + static_cast<ptrdiff_t>(n)));
        frames.erase(frames.begin(), frames.begin() + static_cast<ptrdiff_t>(n));
        size_t bytes = 0;
        for (auto& f : batch) bytes += f.size();
        pending_bytes.fetch_sub(bytes, std::memory_order_relaxed);
        return batch;
    }
};

// ── ConnectionManager::Impl ──────────────────────────────────────────────────

struct ConnectionManager::Impl {
    explicit Impl(SignalBus& bus, NodeIdentity identity, Config* config);
    ~Impl() = default;

    // ── RCU connection registry ─────────────────────────────────────────────

    using RecordMap    = std::unordered_map<conn_id_t, std::shared_ptr<ConnectionRecord>>;
    using RecordMapPtr = std::shared_ptr<const RecordMap>;

    std::mutex              records_write_mu_;
    std::atomic<RecordMapPtr> records_rcu_;

    RecordMapPtr rcu_read() const noexcept {
        return records_rcu_.load(std::memory_order_acquire);
    }

    template<typename Fn>
    void rcu_update(Fn&& fn) {
        auto old = records_rcu_.load(std::memory_order_acquire);
        auto next = std::make_shared<RecordMap>(*old);
        fn(*next);
        records_rcu_.store(std::move(next), std::memory_order_release);
    }

    std::shared_ptr<ConnectionRecord> rcu_find(conn_id_t id) const {
        auto map = rcu_read();
        auto it  = map->find(id);
        return it != map->end() ? it->second : nullptr;
    }

    // ── Per-connection send queues ──────────────────────────────────────────

    mutable std::shared_mutex queues_mu_;
    std::unordered_map<conn_id_t, std::shared_ptr<PerConnQueue>> send_queues_;

    std::shared_ptr<PerConnQueue> get_or_create_queue(conn_id_t id);
    void flush_queue(conn_id_t id, PerConnQueue& q);

    // ── Relay dedup ─────────────────────────────────────────────────────────

    struct RelayFingerprint {
        uint64_t sender_hash;
        uint64_t packet_id;
        std::chrono::steady_clock::time_point ts;
    };
    struct RelayFingerprintHash {
        size_t operator()(const RelayFingerprint& f) const noexcept {
            return std::hash<uint64_t>{}(f.sender_hash) ^
                   (std::hash<uint64_t>{}(f.packet_id) << 1);
        }
    };
    struct RelayFingerprintEq {
        bool operator()(const RelayFingerprint& a, const RelayFingerprint& b) const noexcept {
            return a.sender_hash == b.sender_hash && a.packet_id == b.packet_id;
        }
    };

    std::mutex relay_dedup_mu_;
    std::unordered_set<RelayFingerprint, RelayFingerprintHash, RelayFingerprintEq> relay_dedup_set_;
    bool relay_seen(const header_t* inner_hdr);

    // ── Core refs ───────────────────────────────────────────────────────────

    SignalBus&        bus_;
    Config*           config_ = nullptr;

    mutable std::shared_mutex identity_mu_;
    NodeIdentity              identity_;

    std::atomic<bool>      shutting_down_{false};
    std::atomic<conn_id_t> next_id_{1};
    std::atomic<uint32_t>  in_flight_dispatches_{0}; ///< Shutdown barrier counter

    // ── Registration ────────────────────────────────────────────────────────

    std::vector<std::string> scheme_priority_{"tcp", "ice"};

    mutable std::shared_mutex handlers_mu_;
    std::unordered_map<std::string, HandlerEntry> handler_entries_;

    mutable std::shared_mutex connectors_mu_;
    std::unordered_map<std::string, connector_ops_t*> connectors_;

    // ── Indices ─────────────────────────────────────────────────────────────

    mutable std::shared_mutex uri_mu_;
    std::unordered_map<std::string, conn_id_t> uri_index_;

    mutable std::shared_mutex pk_mu_;
    std::unordered_map<std::string, conn_id_t> pk_index_;

    /// transport_conn_id → peer conn_id (для вторичных транспортов).
    /// Для первичного пути transport_conn_id == ConnectionRecord::id.
    mutable std::shared_mutex transport_mu_;
    std::unordered_map<conn_id_t, conn_id_t> transport_index_;

    /// @brief Индекс приоритета scheme (0 = лучший, 255 = неизвестный).
    uint8_t scheme_priority_index(const std::string& scheme) const {
        for (size_t i = 0; i < scheme_priority_.size(); ++i)
            if (scheme_priority_[i] == scheme) return static_cast<uint8_t>(i);
        return 255;
    }

    // ── Pending messages ────────────────────────────────────────────────────

    mutable std::shared_mutex pending_mu_;
    std::unordered_map<std::string, std::vector<PendingMessage>> pending_messages_;

    // ── Constants ───────────────────────────────────────────────────────────

    static constexpr auto     PENDING_TTL           = std::chrono::seconds(30);
    static constexpr size_t   PENDING_MAX_PER_URI   = 100;
    static constexpr auto     HEARTBEAT_INTERVAL    = std::chrono::seconds(30);
    static constexpr uint32_t MAX_MISSED_HEARTBEATS = 3;
    static constexpr size_t   GLOBAL_MAX_IN_FLIGHT  = 512UL * 1024 * 1024;
    static constexpr size_t   CHUNK_SIZE            = 1UL   * 1024 * 1024;
    static constexpr size_t   MAX_RECV_BUF          = 16UL  * 1024 * 1024; ///< 16 MB per-connection

    // ── Public API implementation ───────────────────────────────────────────

    void register_handler(handler_t* h);
    void register_handler_from_connector(handler_t* h);
    void register_handler_internal(handler_t* h, HandlerSource source);
    static bool is_connector_blocked_type(uint32_t msg_type);
    void register_connector(const std::string& scheme, connector_ops_t* ops);
    void set_scheme_priority(std::vector<std::string> priority);
    void fill_host_api(host_api_t* api);

    bool send(std::string_view uri, uint32_t msg_type, std::span<const uint8_t> payload);
    bool send(conn_id_t id, uint32_t msg_type, std::span<const uint8_t> payload);
    void broadcast(uint32_t msg_type, std::span<const uint8_t> payload);

    void connect(std::string_view uri);
    void disconnect(conn_id_t id);
    void close_now(conn_id_t id);
    void shutdown();

    void rotate_identity_keys(const IdentityConfig& cfg);
    bool rekey_session(conn_id_t id);

    void relay(conn_id_t exclude_conn, uint8_t ttl,
               const uint8_t dest_pubkey[GN_SIGN_PUBLICKEYBYTES],
               std::span<const uint8_t> inner_frame);

    void check_heartbeat_timeouts();
    void cleanup_stale_pending();

    size_t                      connection_count() const;
    std::vector<std::string>    get_active_uris() const;
    std::vector<conn_id_t>      get_active_conn_ids() const;
    std::optional<conn_state_t> get_state(conn_id_t id) const;
    std::optional<std::string>  get_negotiated_scheme(conn_id_t id) const;
    std::optional<std::vector<uint8_t>> get_peer_pubkey(conn_id_t id) const;
    std::string                 get_peer_pubkey_hex(conn_id_t id) const;
    std::optional<endpoint_t>   get_peer_endpoint(conn_id_t id) const;
    conn_id_t                   find_conn_by_pubkey(const char* pubkey_hex) const;
    size_t                      get_pending_bytes(conn_id_t id = CONN_ID_INVALID) const noexcept;
    std::string                 dump_connections() const;

    msg::CoreMeta local_core_meta() const;

    // ── Internal methods ────────────────────────────────────────────────────

    // Heartbeat
    void send_heartbeat(conn_id_t id);
    void handle_heartbeat(conn_id_t id, std::span<const uint8_t> payload);

    // Relay
    void handle_relay(conn_id_t id, std::span<const uint8_t> plaintext);

    // Connection callbacks
    conn_id_t handle_connect(const endpoint_t* ep);
    conn_id_t handle_add_transport(const char* pubkey_hex,
                                    const endpoint_t* ep, const char* scheme);
    void      handle_disconnect(conn_id_t id, int error);
    void      handle_data(conn_id_t id, const void* raw, size_t size);
    void      dispatch_packet(conn_id_t id, const header_t* hdr,
                              std::span<const uint8_t> payload, uint64_t recv_ts_ns);

    // Noise handshake
    void send_noise_init(conn_id_t id);
    void handle_noise_init(conn_id_t id, std::span<const uint8_t> payload);
    void handle_noise_resp(conn_id_t id, std::span<const uint8_t> payload);
    void handle_noise_fin (conn_id_t id, std::span<const uint8_t> payload);

    std::vector<uint8_t> build_handshake_payload();
    bool process_handshake_payload(conn_id_t id, const uint8_t* data, size_t len);
    void finalize_handshake(conn_id_t id);
    void schedule_transport_upgrade(conn_id_t id);

    // Transport
    std::vector<uint8_t> build_frame(conn_id_t id, uint32_t msg_type,
                                      std::span<const uint8_t> payload);
    void send_frame(conn_id_t id, uint32_t msg_type, std::span<const uint8_t> payload);
    bool flush_frames_to_connector(conn_id_t id, connector_ops_t* ops,
                                    std::vector<std::vector<uint8_t>>& frames);

    // Helpers
    std::string              negotiate_scheme(const ConnectionRecord& rec) const;
    std::vector<std::string> local_schemes() const;
    std::optional<conn_id_t> resolve_uri(std::string_view uri) const;
    connector_ops_t*         find_connector(const std::string& scheme);
    void                     flush_pending_messages(const std::string& uri, conn_id_t id);

    static bool     is_localhost_address(std::string_view address);
    static uint64_t monotonic_ns() noexcept;

    // ── C-ABI trampolines ───────────────────────────────────────────────────

    static conn_id_t s_on_connect      (void*, const endpoint_t*);
    static void      s_on_data         (void*, conn_id_t, const void*, size_t);
    static void      s_on_disconnect   (void*, conn_id_t, int);
    static void      s_send            (void*, const char*, uint32_t, const void*, size_t);
    static void      s_send_response   (void*, conn_id_t, uint32_t, const void*, size_t);
    static void      s_broadcast       (void*, uint32_t, const void*, size_t);
    static void      s_disconnect      (void*, conn_id_t);
    static int       s_sign            (void*, const void*, size_t, uint8_t[GN_SIGN_BYTES]);
    static int       s_verify          (void*, const void*, size_t, const uint8_t*, const uint8_t*);
    static conn_id_t s_find_conn_by_pk (void*, const char*);
    static int       s_get_peer_info   (void*, conn_id_t, endpoint_t*);
    static int       s_config_get      (void*, const char*, char*, size_t);
    static void      s_register_handler(void*, handler_t*);
    static conn_id_t s_add_transport   (void*, const char*, const endpoint_t*, const char*);
    static void      s_log             (void*, int, const char*, int, const char*);
};

} // namespace gn
