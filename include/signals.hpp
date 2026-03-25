#pragma once
/// @file include/signals.hpp
/// @brief Event dispatch system: packet pipeline, async signals, traffic stats.
///
/// ## Components
///   - **PipelineSignal** — lock-free ordered handler chain for packet dispatch.
///     Handlers are invoked by priority; chain stops on CONSUMED/REJECT.
///   - **EventSignal<Args...>** — async event broadcast via io_context strand.
///   - **SignalBus** — facade combining pipelines, events, and stats accumulation.
///
/// ## Stats accumulation
/// All counters use `std::atomic` with relaxed ordering — designed for
/// high-throughput hot paths.  `stats_snapshot()` provides a consistent read.
///
/// ## Thread-safety
/// - PipelineSignal: `emit()` is wait-free (atomic shared_ptr read).
///   `connect()`/`disconnect()` take a mutex (rare path).
/// - EventSignal: `emit()` posts to a strand — handlers run sequentially.
/// - SignalBus: packet dispatch uses shared_mutex for channel map access.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "../sdk/cpp/data.hpp"

namespace boost::asio { class io_context; }

namespace gn {

// ── Drop reasons ──────────────────────────────────────────────────────────────

/// @brief Why a packet was dropped.  Used for stats and diagnostics.
/// Each value maps to a counter in StatsSnapshot::drops[].
enum class DropReason : uint8_t {
    BadMagic            = 0,
    BadProtoVer         = 1,
    ConnNotFound        = 2,
    StateNotEstablished = 3,
    AuthFail            = 4,
    DecryptFail         = 5,
    ReplayDetected      = 6,
    Backpressure        = 7,
    PerConnLimitExceeded= 8,
    SessionNotReady     = 9,
    RejectedByHandler   = 10,
    ShuttingDown        = 11,
    RelayDropped        = 12,
    SenderIdMismatch    = 13,
    TrustedFromRemote   = 14,
    RecvBufOverflow     = 15,  ///< recv_buf exceeded MAX_RECV_BUF → close
    ConnectorNotFound   = 16,  ///< send_frame: no connector for negotiated scheme
    _Count              = 17,
};

// ── Latency histogram ─────────────────────────────────────────────────────────

/// @brief Lock-free latency histogram with 7 exponential buckets (1us–100ms+).
///
/// All operations are atomic (relaxed ordering) — safe for concurrent recording
/// from multiple IO threads without any locking overhead.
struct LatencyHistogram {
    static constexpr uint64_t BUCKET_NS[7] = {
        1'000, 10'000, 100'000, 1'000'000,
        10'000'000, 100'000'000, UINT64_MAX
    };

    std::atomic<uint64_t> buckets[7]{};
    std::atomic<uint64_t> total_ns{0};
    std::atomic<uint64_t> count{0};

    LatencyHistogram() = default;

    // std::atomic non-copyable → explicit copy/move for snapshot semantics.
    LatencyHistogram(const LatencyHistogram& o) noexcept { *this = o; }
    LatencyHistogram& operator=(const LatencyHistogram& o) noexcept {
        for (int i = 0; i < 7; ++i)
            buckets[i].store(o.buckets[i].load(std::memory_order_relaxed),
                             std::memory_order_relaxed);
        total_ns.store(o.total_ns.load(std::memory_order_relaxed),
                       std::memory_order_relaxed);
        count.store(o.count.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
        return *this;
    }
    LatencyHistogram(LatencyHistogram&& o) noexcept            { *this = o; }
    LatencyHistogram& operator=(LatencyHistogram&& o) noexcept { return *this = o; }

    void record(uint64_t ns) noexcept {
        total_ns.fetch_add(ns, std::memory_order_relaxed);
        count   .fetch_add(1,  std::memory_order_relaxed);
        for (int i = 0; i < 7; ++i) {
            if (ns < BUCKET_NS[i]) {
                buckets[i].fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
    }

    [[nodiscard]] uint64_t avg_ns() const noexcept {
        auto c = count.load(std::memory_order_relaxed);
        return c ? total_ns.load(std::memory_order_relaxed) / c : 0;
    }
};

// ── Stats ─────────────────────────────────────────────────────────────────────

/// @brief Single stat event emitted by the core on every packet/connection action.
struct StatsEvent {
    enum class Kind : uint8_t {
        RxBytes, TxBytes, RxPacket, TxPacket,
        AuthOk, AuthFail,
        DecryptFail, Backpressure,
        Consumed, Rejected,
        Connect, Disconnect,
        Drop,
        DispatchLatencyNs,
    };
    Kind       kind;
    uint64_t   value   = 1;
    conn_id_t  conn_id = CONN_ID_INVALID;
    DropReason drop_reason{};
};

/// @brief Atomic snapshot of accumulated traffic, auth, and drop counters.
///
/// Obtained via `SignalBus::stats_snapshot()` or `Core::stats_snapshot()`.
/// All values are monotonically increasing (except `connections` which is current count).
struct StatsSnapshot {
    uint64_t rx_bytes     = 0;
    uint64_t tx_bytes     = 0;
    uint64_t rx_packets   = 0;
    uint64_t tx_packets   = 0;
    uint64_t auth_ok      = 0;
    uint64_t auth_fail    = 0;
    uint64_t decrypt_fail = 0;
    uint64_t backpressure = 0;
    uint64_t consumed     = 0;
    uint64_t rejected     = 0;
    uint32_t connections  = 0;
    uint32_t total_conn   = 0;
    uint32_t total_disc   = 0;

    uint64_t drops[static_cast<size_t>(DropReason::_Count)]{};
    LatencyHistogram dispatch_latency;
};

// ── PipelineSignal ────────────────────────────────────────────────────────────

using HandlerPacketFn = std::function<
    propagation_t(std::string_view name,
                  std::shared_ptr<header_t> hdr,
                  const endpoint_t*         ep,
                  PacketData                data)>;

/// @brief Lock-free ordered packet dispatch chain.
///
/// Handlers are sorted by priority (0 = highest).  `emit()` iterates the
/// snapshot via `std::atomic<shared_ptr>` — wait-free on the read path.
/// `connect()`/`disconnect()` rebuild the vector under a mutex (rare path).
///
/// Used by SignalBus for per-message-type channels and the wildcard channel.
class PipelineSignal {
public:
    PipelineSignal()
        : handlers_ptr_(std::make_shared<const std::vector<Entry>>()) {}

    /// @brief Add a handler to the pipeline. Rebuilds the sorted vector.
    void connect   (uint8_t priority, std::string_view name, HandlerPacketFn fn);
    /// @brief Remove a handler by name.
    void disconnect(std::string_view name);

    struct EmitResult {
        propagation_t result      = PROPAGATION_CONTINUE;
        std::string   consumed_by;
    };

    EmitResult emit(std::shared_ptr<header_t> hdr,
                    const endpoint_t*         ep,
                    PacketData                data) const;

private:
    struct Entry { uint8_t priority; std::string name; HandlerPacketFn fn; };
    mutable std::mutex write_mu_;
    std::atomic<std::shared_ptr<const std::vector<Entry>>> handlers_ptr_;
};

// ── EventSignal ───────────────────────────────────────────────────────────────

/// @brief Base class that owns a strand for serialized event delivery.
class EventSignalBase {
public:
    explicit EventSignalBase(boost::asio::io_context& ioc);
    virtual ~EventSignalBase();

protected:
    bool try_post_to_strand(std::function<void()> task);

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

/// @brief Type-safe async event signal.
///
/// Handlers are invoked via an io_context strand — guaranteed sequential
/// execution regardless of which thread calls `emit()`.
template<typename... Args>
class EventSignal : public EventSignalBase {
public:
    using Handler = std::function<void(Args...)>;
    explicit EventSignal(boost::asio::io_context& ioc) : EventSignalBase(ioc) {}

    void connect(Handler h) {
        std::lock_guard lock(mu_);
        handlers_.push_back(std::move(h));
    }

    void emit(Args... args) {
        std::unique_lock lock(mu_);
        auto snap = handlers_;
        lock.unlock();
        for (auto& h : snap)
            try_post_to_strand([h, args...]() mutable {
                try { h(args...); } catch (...) {}
            });
    }

private:
    mutable std::mutex   mu_;
    std::vector<Handler> handlers_;
};

// ── SignalBus ─────────────────────────────────────────────────────────────────

/// @brief Central dispatcher: packet pipeline, async events, stats accumulation.
///
/// Owns per-message-type PipelineSignal channels and a wildcard channel.
/// Packets are dispatched through the type-specific chain first, then the
/// wildcard chain.  Stats are accumulated atomically for zero-allocation
/// hot-path accounting.
///
/// ## Subscription model
///   - `subscribe(msg_type, ...)` → per-type channel
///   - `subscribe_wildcard(...)` → receives ALL message types
///   - Each subscription gets a unique `sub_id` for later `unsubscribe()`.
class SignalBus {
public:
    explicit SignalBus(boost::asio::io_context& ioc);

    /// @name Packet pipeline
    /// @{

    /// @brief Subscribe a handler to a specific message type.
    /// @return Subscription ID for unsubscribe().
    uint64_t subscribe(uint32_t msg_type, std::string_view name,
                       HandlerPacketFn cb, uint8_t prio = 128);

    /// @brief Subscribe to all message types (wildcard).
    void subscribe_wildcard(std::string_view name,
                            HandlerPacketFn cb, uint8_t prio = 128);

    /// @brief Remove a subscription by ID.
    void unsubscribe(uint64_t sub_id);

    /// @brief Dispatch a packet through the handler chain.
    /// @return Which handler consumed it (or CONTINUE if none did).
    PipelineSignal::EmitResult dispatch_packet(uint32_t                  msg_type,
                                               std::shared_ptr<header_t> hdr,
                                               const endpoint_t*         ep,
                                               PacketData                data);
    /// @}

    /// @name Stats accumulation (lock-free, relaxed atomics)
    /// @{
    void emit_stat   (StatsEvent ev)                noexcept;
    void emit_drop   (conn_id_t id, DropReason why) noexcept;
    void emit_latency(conn_id_t id, uint64_t ns)    noexcept;

    /// @brief Read a consistent snapshot of all accumulated counters.
    [[nodiscard]] StatsSnapshot stats_snapshot() const noexcept;
    /// @}

    /// @name Async event signals (strand-serialized delivery)
    /// @{
    EventSignal<StatsEvent>              on_stat;        ///< Every stat event
    EventSignal<std::string>             on_log;         ///< Log messages
    EventSignal<conn_id_t, conn_state_t> on_conn_state;  ///< Connection state changes
    EventSignal<conn_id_t, std::string, bool> on_transport_change; ///< (peer_id, scheme, added)
    /// @}

private:
    mutable std::shared_mutex mu_;
    std::unordered_map<uint32_t, std::unique_ptr<PipelineSignal>> channels_;
    PipelineSignal wildcards_;

    struct SubInfo { uint32_t msg_type; std::string name; };
    mutable std::shared_mutex sub_mu_;
    std::atomic<uint64_t>     next_sub_id_{1};
    std::unordered_map<uint64_t, SubInfo> sub_map_;

    struct Accum {
        std::atomic<uint64_t> rx_bytes{0}, tx_bytes{0};
        std::atomic<uint64_t> rx_packets{0}, tx_packets{0};
        std::atomic<uint64_t> auth_ok{0}, auth_fail{0};
        std::atomic<uint64_t> decrypt_fail{0}, backpressure{0};
        std::atomic<uint64_t> consumed{0}, rejected{0};
        std::atomic<uint32_t> connections{0}, total_conn{0}, total_disc{0};
        std::atomic<uint64_t> drops[static_cast<size_t>(DropReason::_Count)]{};
        LatencyHistogram       dispatch_lat;
    } accum_;
};

} // namespace gn
