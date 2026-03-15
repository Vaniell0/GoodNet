#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "signals.hpp"
#include "types/connection.hpp"
#include "types/identify.hpp"

#include "../sdk/connector.h"
#include "../sdk/handler.h"
#include "../sdk/plugin.h"

namespace gn {

// ── PerConnQueue ──────────────────────────────────────────────────────────────

/// Per-connection outbound frame queue with independent backpressure limit.
struct PerConnQueue {
    static constexpr size_t MAX_BYTES = 8 * 1024 * 1024;  // 8 MB per-conn

    std::mutex               mu;
    std::vector<std::vector<uint8_t>> frames;
    std::atomic<size_t>      pending_bytes{0};
    std::atomic<bool>        draining{false};   ///< set during graceful close

    /// @brief Atomically reserve space and enqueue a frame.
    /// @return false if backpressure limit would be exceeded.
    bool try_push(std::vector<uint8_t> frame) {
        const size_t sz = frame.size();
        // Atomic fetch_add with rollback — no TOCTOU window between check and add.
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

// ── ConnectionManager ─────────────────────────────────────────────────────────

/// Owns all peer connections: handshake, encryption, framing, dispatch.
///
/// Connection registry uses an RCU-style atomic<shared_ptr<const RecordMap>>:
/// hot read-path is mutex-free (one atomic load), writes take a per-CM mutex
/// and do a copy-and-swap.
class ConnectionManager {
public:
    explicit ConnectionManager(SignalBus& bus, NodeIdentity identity);
    ~ConnectionManager();

    ConnectionManager(const ConnectionManager&)            = delete;
    ConnectionManager& operator=(const ConnectionManager&) = delete;

    // ── Registration ─────────────────────────────────────────────────────────

    void register_connector   (const std::string& scheme, connector_ops_t* ops);
    void register_handler     (handler_t* h);
    void set_scheme_priority  (std::vector<std::string> priority);

    /// Populate a host_api_t vtable with all CM callbacks.
    void fill_host_api(host_api_t* api);

    // ── Send ─────────────────────────────────────────────────────────────────

    /// Resolve URI → conn, encrypt, and enqueue frame. Returns false on
    /// backpressure, unknown URI, or non-ESTABLISHED state.
    bool send(const char* uri, uint32_t msg_type,
              std::span<const uint8_t> payload);

    bool send(const char* uri, uint32_t msg_type,
              const void* payload, size_t size) {
        return send(uri, msg_type,
                    std::span<const uint8_t>(
                        static_cast<const uint8_t*>(payload), size));
    }

    /// Send directly on an existing connection ID.
    bool send_on_conn(conn_id_t id, uint32_t msg_type,
                      std::span<const uint8_t> payload);

    bool send_on_conn(conn_id_t id, uint32_t msg_type,
                      const void* p, size_t sz) {
        return send_on_conn(id, msg_type,
                            std::span<const uint8_t>(
                                static_cast<const uint8_t*>(p), sz));
    }

    /// Broadcast to all ESTABLISHED peers.
    void broadcast(uint32_t msg_type, std::span<const uint8_t> payload);

    void broadcast(uint32_t msg_type, const void* p, size_t sz) {
        broadcast(msg_type,
                  std::span<const uint8_t>(static_cast<const uint8_t*>(p), sz));
    }

    // ── Connection control ────────────────────────────────────────────────────

    /// Initiate outbound connection (non-blocking; result via on_connect).
    void connect(std::string_view uri);

    /// Begin graceful close: flush PerConnQueue, then close.
    void disconnect(conn_id_t id);

    /// Hard close (no drain).
    void close_now(conn_id_t id);

    void shutdown();

    // ── Key operations ────────────────────────────────────────────────────────

    /// Rotate long-term identity keys. Existing sessions keep their session keys
    /// — no disruption to established peers (PFS guarantees this is safe).
    void rotate_identity_keys(const IdentityConfig& cfg);

    /// Re-derive a fresh session key for an ESTABLISHED connection without
    /// dropping it (sends MSG_TYPE_KEY_EXCHANGE, awaits peer response).
    bool rekey_session(conn_id_t id);

    // ── Relay ─────────────────────────────────────────────────────────────────

    /// Send inner_frame via gossip relay to dest_pubkey through all peers.
    void relay(conn_id_t exclude_conn, uint8_t ttl,
               const uint8_t dest_pubkey[32],
               std::span<const uint8_t> inner_frame);

    // ── Queries ───────────────────────────────────────────────────────────────

    [[nodiscard]] size_t                      connection_count()              const;
    [[nodiscard]] std::vector<std::string>    get_active_uris()               const;
    [[nodiscard]] std::vector<conn_id_t>     get_active_conn_ids()           const;
    [[nodiscard]] std::optional<conn_state_t> get_state(conn_id_t id)         const;
    [[nodiscard]] std::optional<std::string>  get_negotiated_scheme(conn_id_t id) const;
    [[nodiscard]] std::optional<std::vector<uint8_t>> get_peer_pubkey(conn_id_t id) const;
    [[nodiscard]] bool get_peer_endpoint(conn_id_t id, endpoint_t& out)       const;
    [[nodiscard]] conn_id_t find_conn_by_pubkey(const char* pubkey_hex)        const;

    [[nodiscard]] size_t get_pending_bytes(conn_id_t id = CONN_ID_INVALID)    const noexcept;

    [[nodiscard]] const NodeIdentity& identity() const { return identity_; }

    msg::CoreMeta local_core_meta() const;

private:
    // ── RCU connection registry ───────────────────────────────────────────────

    using RecordMap = std::unordered_map<conn_id_t, std::shared_ptr<ConnectionRecord>>;
    using RecordMapPtr = std::shared_ptr<const RecordMap>;

    std::mutex       records_write_mu_;   // serialises writers
    std::atomic<RecordMapPtr> records_rcu_;   // readers: one atomic load, no lock

    RecordMapPtr rcu_read() const noexcept {
        return records_rcu_.load(std::memory_order_acquire);
    }

    /// Copy-and-swap: fn(mutable ref to new map). Must be called under records_write_mu_.
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

    // ── Per-connection send queues ─────────────────────────────────────────────

    mutable std::shared_mutex queues_mu_;
    std::unordered_map<conn_id_t, std::shared_ptr<PerConnQueue>> send_queues_;

    std::shared_ptr<PerConnQueue> get_or_create_queue(conn_id_t id);
    void flush_queue(conn_id_t id, PerConnQueue& q);

    // ── Relay (private) ───────────────────────────────────────────────────────

    void handle_relay(conn_id_t id, std::span<const uint8_t> plaintext);

    static constexpr size_t RELAY_DEDUP_SIZE = 4096;
    struct RelayFingerprint {
        uint64_t sender_hash;
        uint64_t packet_id;
    };
    std::mutex relay_dedup_mu_;
    std::array<RelayFingerprint, RELAY_DEDUP_SIZE> relay_dedup_{};
    size_t relay_dedup_pos_ = 0;
    bool relay_seen(const header_t* inner_hdr);

    // ── Internal callbacks ────────────────────────────────────────────────────

    conn_id_t handle_connect   (const endpoint_t* ep);
    void      handle_disconnect(conn_id_t id, int error);
    void      handle_data      (conn_id_t id, const void* raw, size_t size);
    void      dispatch_packet  (conn_id_t id, const header_t* hdr,
                                 std::span<const uint8_t> payload,
                                 uint64_t recv_ts_ns);

    void send_auth    (conn_id_t id);
    bool process_auth (conn_id_t id, std::span<const uint8_t> payload);
    bool process_keyex(conn_id_t id, std::span<const uint8_t> payload);
    bool derive_session(conn_id_t id,
                        const uint8_t peer_ephem_pk[32],
                        const uint8_t peer_user_pk [32]);

    /// Build a complete wire frame (header + optionally encrypted payload).
    /// Returns empty on error.
    std::vector<uint8_t> build_frame(conn_id_t id, uint32_t msg_type,
                                      std::span<const uint8_t> payload);

    void send_frame(conn_id_t id, uint32_t msg_type,
                    std::span<const uint8_t> payload);

    /// Try gather-IO: connector->send_gather() if available, else loop send_to.
    void flush_frames_to_connector(conn_id_t id, connector_ops_t* ops,
                                    std::vector<std::vector<uint8_t>>& frames);

    std::string      negotiate_scheme(const ConnectionRecord& rec)  const;
    std::vector<std::string> local_schemes()                         const;
    std::optional<conn_id_t> resolve_uri(std::string_view uri)       const;
    connector_ops_t* find_connector  (const std::string& scheme);
    static bool      is_localhost_address(std::string_view address);

    static uint64_t  monotonic_ns() noexcept;

    // ── C-ABI trampolines ─────────────────────────────────────────────────────

    static conn_id_t s_on_connect      (void*, const endpoint_t*);
    static void      s_on_data         (void*, conn_id_t, const void*, size_t);
    static void      s_on_disconnect   (void*, conn_id_t, int);
    static void      s_send            (void*, const char*, uint32_t, const void*, size_t);
    static void      s_send_response   (void*, conn_id_t, uint32_t, const void*, size_t);
    static void      s_broadcast       (void*, uint32_t, const void*, size_t);
    static void      s_disconnect      (void*, conn_id_t);
    static int       s_sign            (void*, const void*, size_t, uint8_t[64]);
    static int       s_verify          (void*, const void*, size_t, const uint8_t*, const uint8_t*);
    static conn_id_t s_find_conn_by_pk (void*, const char*);
    static int       s_get_peer_info   (void*, conn_id_t, endpoint_t*);
    static int       s_config_get      (void*, const char*, char*, size_t);
    static void      s_register_handler(void*, handler_t*);
    static void      s_log             (void*, int, const char*, int, const char*);

    // ── Members ───────────────────────────────────────────────────────────────

    SignalBus&         bus_;

    mutable std::shared_mutex identity_mu_;
    NodeIdentity               identity_;

    std::atomic<bool>  shutting_down_{false};
    std::atomic<conn_id_t> next_id_{1};

    std::vector<std::string> scheme_priority_{"tcp", "ice", "udp", "mock"};

    mutable std::shared_mutex handlers_mu_;
    std::unordered_map<std::string, HandlerEntry> handler_entries_;

    mutable std::shared_mutex connectors_mu_;
    std::unordered_map<std::string, connector_ops_t*> connectors_;

    mutable std::shared_mutex uri_mu_;
    std::unordered_map<std::string, conn_id_t> uri_index_;

    mutable std::shared_mutex pk_mu_;
    std::unordered_map<std::string, conn_id_t> pk_index_;

    static constexpr size_t GLOBAL_MAX_IN_FLIGHT = 512UL * 1024 * 1024;
    static constexpr size_t CHUNK_SIZE           = 1UL   * 1024 * 1024;
};

} // namespace gn
