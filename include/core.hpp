#pragma once

/// @file include/core.hpp
/// @brief GoodNet public C++ API.
///
/// Only stdlib and signals.hpp (for StatsSnapshot) are included here.
/// All heavy dependencies live in core.cpp behind the Pimpl.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../sdk/cpp/data.hpp"
#include "signals.hpp"

namespace gn {

class ConnectionManager;
class PluginManager;
class SignalBus;
struct NodeIdentity;

namespace fs = std::filesystem;

// ── CoreConfig ────────────────────────────────────────────────────────────────

struct CoreConfig {
    struct {
        fs::path dir            = "~/.goodnet";
        fs::path ssh_key_path;
        bool     use_machine_id = true;
    } identity;

    struct {
        std::vector<fs::path> dirs;
        bool auto_load = true;
    } plugins;

    struct {
        std::string listen_address = "0.0.0.0";
        uint16_t    listen_port    = 25565;
        int         io_threads     = 0;   ///< 0 = hardware_concurrency()
    } network;

    struct {
        std::string level     = "info";
        std::string file;
        size_t      max_size  = 10 * 1024 * 1024;
        int         max_files = 5;
    } logging;

    fs::path config_file;
};

// ── Core ──────────────────────────────────────────────────────────────────────

class Core {
public:
    using PacketHandler = std::function<propagation_t(
        std::string_view          name,
        std::shared_ptr<header_t> hdr,
        const endpoint_t*         ep,
        PacketData                data)>;

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    explicit Core(CoreConfig cfg = {});
    ~Core();

    Core(const Core&)            = delete;
    Core& operator=(const Core&) = delete;

    void run();
    void run_async(int threads = 0);
    void stop();
    [[nodiscard]] bool is_running() const noexcept;

    // ── Network ───────────────────────────────────────────────────────────────

    bool send(const char* uri, uint32_t msg_type,
              const void* payload, size_t size);
    bool send(const char* uri, uint32_t msg_type,
              std::string_view payload);
    bool send(const char* uri, uint32_t msg_type,
              std::span<const uint8_t> payload);

    bool send_to(conn_id_t id, uint32_t msg_type,
                 const void* payload, size_t size);
    bool send_to(conn_id_t id, uint32_t msg_type,
                 std::string_view payload);
    bool send_to(conn_id_t id, uint32_t msg_type,
                 std::span<const uint8_t> payload);

    void broadcast(uint32_t msg_type, const void* payload, size_t size);
    void broadcast(uint32_t msg_type, std::string_view payload);
    void broadcast(uint32_t msg_type, std::span<const uint8_t> payload);

    void connect   (std::string_view uri);
    void disconnect(conn_id_t id);
    void close_now (conn_id_t id);

    // ── Key management ────────────────────────────────────────────────────────

    /// Re-derive session key for an ESTABLISHED connection without disconnect.
    bool rekey_session(conn_id_t id);

    /// Rotate long-term identity keys. Existing sessions are unaffected (PFS).
    void rotate_identity_keys();

    // ── Peer info ─────────────────────────────────────────────────────────────

    [[nodiscard]] std::vector<uint8_t> peer_pubkey(conn_id_t id) const;
    [[nodiscard]] bool peer_endpoint(conn_id_t id, endpoint_t& out) const;

    // ── Subscriptions ─────────────────────────────────────────────────────────

    uint64_t subscribe(uint32_t msg_type, std::string_view name,
                       PacketHandler cb, uint8_t prio = 128);
    void     subscribe_wildcard(std::string_view name,
                                PacketHandler cb, uint8_t prio = 128);
    void     unsubscribe(uint64_t sub_id);

    // ── Identity ──────────────────────────────────────────────────────────────

    [[nodiscard]] std::string user_pubkey_hex()   const;
    [[nodiscard]] std::string device_pubkey_hex() const;

    // ── Stats ─────────────────────────────────────────────────────────────────

    [[nodiscard]] StatsSnapshot       stats_snapshot()   const noexcept;
    [[nodiscard]] size_t              connection_count() const noexcept;
    [[nodiscard]] std::vector<std::string> active_uris() const;
    [[nodiscard]] std::vector<conn_id_t>  active_conn_ids() const;

    // ── Config ────────────────────────────────────────────────────────────────

    bool reload_config();

    // ── Internal (CLI / tests) ────────────────────────────────────────────────

    [[nodiscard]] ConnectionManager& cm()  noexcept;
    [[nodiscard]] PluginManager&     pm()  noexcept;
    [[nodiscard]] SignalBus&         bus() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace gn
