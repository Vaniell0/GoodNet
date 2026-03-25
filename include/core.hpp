#pragma once

/// @file include/core.hpp
/// @brief GoodNet public C++ API.
///
/// Lightweight header: only stdlib + sdk types.
/// All heavy dependencies live behind the Pimpl in core.cpp.

#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "../sdk/cpp/data.hpp"
#include "signals.hpp"

class Config;

namespace gn {

class ConnectionManager;
class PluginManager;
class SignalBus;
struct NodeIdentity;

// ── Concepts ──────────────────────────────────────────────────────────────────

/// Contiguous byte-sized range sendable on the wire.
/// Matches: span<const uint8_t>, vector<uint8_t>, string, string_view, etc.
template<typename T>
concept BytePayload = !std::derived_from<std::remove_cvref_t<T>, sdk::IData>
    && requires(const T& t) {
        { t.data() };
        { t.size() } -> std::convertible_to<size_t>;
        requires sizeof(*t.data()) == 1;
    };

/// Serializable message type (anything derived from sdk::IData).
/// Matches: PodData<T>, VarData, custom IData subclasses.
template<typename T>
concept Serializable = std::derived_from<std::remove_cvref_t<T>, sdk::IData>;

// ── Core ──────────────────────────────────────────────────────────────────────

/// @brief Main entry point for the GoodNet network framework.
///
/// Owns the IO context, plugin system, connection manager, and signal bus.
/// Thread-safe: all public methods may be called from any thread.
class Core {
public:
    using PacketHandler = std::function<propagation_t(
        std::string_view          name,
        std::shared_ptr<header_t> hdr,
        const endpoint_t*         ep,
        PacketData                data)>;

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    /// @brief Construct a Core instance.
    /// @param config  External config (borrowed, caller owns lifetime).
    ///                Pass nullptr for an internal defaults-only config.
    explicit Core(Config* config = nullptr);
    ~Core();

    Core(const Core&)            = delete;
    Core& operator=(const Core&) = delete;

    /// @brief Run the IO loop on the calling thread (blocks until stop()).
    void run();

    /// @brief Start IO threads in the background.
    /// @param threads  Number of threads. 0 = auto from config or hardware concurrency.
    void run_async(int threads = 0);

    /// @brief Stop all IO threads and unload plugins.
    void stop();

    /// @brief Check whether the core is currently running.
    [[nodiscard]] bool is_running() const noexcept;

    // ── Send (by URI) ────────────────────────────────────────────────────────

    /// @brief Send raw bytes to a peer by URI.
    /// @details Auto-connects if no existing connection exists.
    ///          Queues the message if the connection is not yet ESTABLISHED.
    /// @param uri       Peer address (e.g. "tcp://host:port").
    /// @param msg_type  Wire message type (MSG_TYPE_*).
    /// @param payload   Raw payload bytes.
    /// @return false on backpressure or invalid URI.
    bool send(std::string_view uri, uint32_t msg_type,
              std::span<const uint8_t> payload);

    /// @brief Send a contiguous byte range to a peer by URI.
    /// @tparam P  BytePayload type (vector<uint8_t>, string, string_view, span, etc.).
    template<BytePayload P>
    bool send(std::string_view uri, uint32_t msg_type, const P& payload) {
        return send(uri, msg_type, as_bytes(payload));
    }

    /// @brief Send a serializable IData message to a peer by URI.
    /// @tparam T  Serializable type (PodData<S>, VarData, custom IData subclass).
    template<Serializable T>
    bool send(std::string_view uri, uint32_t msg_type, const T& data) {
        auto buf = data.serialize();
        return send(uri, msg_type, std::span<const uint8_t>{buf});
    }

    // ── Send (by connection ID) ──────────────────────────────────────────────

    /// @brief Send raw bytes on an existing connection.
    /// @param id        Connection identifier.
    /// @param msg_type  Wire message type.
    /// @param payload   Raw payload bytes.
    /// @return false if connection not found.
    bool send(conn_id_t id, uint32_t msg_type,
              std::span<const uint8_t> payload);

    /// @brief Send a contiguous byte range on an existing connection.
    template<BytePayload P>
    bool send(conn_id_t id, uint32_t msg_type, const P& payload) {
        return send(id, msg_type, as_bytes(payload));
    }

    /// @brief Send a serializable IData message on an existing connection.
    template<Serializable T>
    bool send(conn_id_t id, uint32_t msg_type, const T& data) {
        auto buf = data.serialize();
        return send(id, msg_type, std::span<const uint8_t>{buf});
    }

    // ── Broadcast ─────────────────────────────────────────────────────────────

    /// @brief Broadcast raw bytes to all ESTABLISHED peers.
    /// @param msg_type  Wire message type.
    /// @param payload   Raw payload bytes.
    void broadcast(uint32_t msg_type, std::span<const uint8_t> payload);

    /// @brief Broadcast a contiguous byte range to all ESTABLISHED peers.
    template<BytePayload P>
    void broadcast(uint32_t msg_type, const P& payload) {
        broadcast(msg_type, as_bytes(payload));
    }

    /// @brief Broadcast a serializable IData message to all ESTABLISHED peers.
    template<Serializable T>
    void broadcast(uint32_t msg_type, const T& data) {
        auto buf = data.serialize();
        broadcast(msg_type, std::span<const uint8_t>{buf});
    }

    // ── Connection control ────────────────────────────────────────────────────

    /// @brief Initiate outbound connection (non-blocking).
    void connect   (std::string_view uri);

    /// @brief Begin graceful close: flush send queue, then disconnect.
    void disconnect(conn_id_t id);

    /// @brief Hard close without draining the send queue.
    void close_now (conn_id_t id);

    // ── Key management ────────────────────────────────────────────────────────

    /// @brief Re-derive session key for an ESTABLISHED connection (no disconnect).
    /// @return false if connection not found or not ESTABLISHED.
    bool rekey_session(conn_id_t id);

    /// @brief Rotate long-term identity keys. Existing sessions unaffected (PFS).
    void rotate_identity_keys();

    // ── Peer info ─────────────────────────────────────────────────────────────

    /// @brief Get the Ed25519 public key of a connected peer (raw bytes).
    /// @return Empty vector if connection not found or handshake incomplete.
    [[nodiscard]] std::vector<uint8_t> peer_pubkey(conn_id_t id) const;

    /// @brief Get the Ed25519 public key of a connected peer (hex string).
    /// @return Empty string if connection not found or handshake incomplete.
    [[nodiscard]] std::string peer_pubkey_hex(conn_id_t id) const;

    /// @brief Get the peer's endpoint descriptor.
    /// @return nullopt if connection not found.
    [[nodiscard]] std::optional<endpoint_t> peer_endpoint(conn_id_t id) const;

    // ── Subscriptions ─────────────────────────────────────────────────────────

    /// @brief Subscribe to a specific message type.
    /// @param msg_type  Wire message type to listen for.
    /// @param name      Handler name (for logging / unsubscribe).
    /// @param cb        Callback invoked on each matching packet.
    /// @param prio      Priority (lower = earlier in pipeline). Default 128.
    /// @return Subscription ID for unsubscribe().
    uint64_t subscribe(uint32_t msg_type, std::string_view name,
                       PacketHandler cb, uint8_t prio = 128);

    /// @brief Subscribe to all message types (wildcard).
    void     subscribe_wildcard(std::string_view name,
                                PacketHandler cb, uint8_t prio = 128);

    /// @brief Remove a subscription by ID.
    void     unsubscribe(uint64_t sub_id);

    // ── Identity ──────────────────────────────────────────────────────────────

    /// @brief Hex-encoded Ed25519 user public key.
    [[nodiscard]] std::string user_pubkey_hex()   const;

    /// @brief Hex-encoded Ed25519 device public key.
    [[nodiscard]] std::string device_pubkey_hex() const;

    // ── Stats ─────────────────────────────────────────────────────────────────

    /// @brief Atomic snapshot of accumulated traffic / auth / drop counters.
    [[nodiscard]] StatsSnapshot       stats_snapshot()   const noexcept;

    /// @brief Number of currently active connections (any state).
    [[nodiscard]] size_t              connection_count() const noexcept;

    /// @brief URIs of all active connections.
    [[nodiscard]] std::vector<std::string> active_uris() const;

    /// @brief Connection IDs of all active connections.
    [[nodiscard]] std::vector<conn_id_t>  active_conn_ids() const;

    /// @brief JSON diagnostic dump of all active connections.
    [[nodiscard]] std::string dump_connections() const;

    // ── Plugin info ────────────────────────────────────────────────────────────

    /// @brief Number of enabled handler plugins.
    [[nodiscard]] size_t handler_count()   const noexcept;

    /// @brief Number of enabled connector plugins.
    [[nodiscard]] size_t connector_count() const noexcept;

    // ── Config ────────────────────────────────────────────────────────────────

    /// @brief Hot-reload config from disk. Updates log level.
    bool reload_config();

    // ── Internal (CLI / tests) ────────────────────────────────────────────────

    [[nodiscard]] ConnectionManager& cm()  noexcept;
    [[nodiscard]] PluginManager&     pm()  noexcept;
    [[nodiscard]] SignalBus&         bus() noexcept;

private:
    void start_heartbeat_timer();

    /// Helper: convert any BytePayload-compatible container to span<const uint8_t>.
    template<BytePayload P>
    static std::span<const uint8_t> as_bytes(const P& p) {
        return {reinterpret_cast<const uint8_t*>(p.data()), p.size()};
    }

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace gn
