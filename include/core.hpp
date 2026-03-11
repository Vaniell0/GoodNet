#pragma once
/// @file include/core.hpp
/// @brief GoodNet public API — intentionally minimal.
///
/// This header includes ONLY: stdlib (<memory>, <functional>, <string>,
/// <string_view>, <vector>, <filesystem>, <expected>) and sdk/types.h (the
/// plain-C ABI types shared with plugins).
///
/// Boost, libsodium, spdlog, fmt, nlohmann_json, ConnectionManager,
/// PluginManager are ALL hidden behind the Pimpl.  Downstream code that
/// includes this header compiles in milliseconds and leaks nothing.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "../sdk/types.h"   // propagation_t, header_t, endpoint_t, msg types — lightweight C header

namespace gn {

// ─── Forward declarations — never include their headers here ─────────────────
class ConnectionManager;
class PluginManager;
class SignalBus;
struct NodeIdentity;

namespace fs = std::filesystem;

// ─── CoreConfig ───────────────────────────────────────────────────────────────
// Plain-data struct, no dependencies.

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

    fs::path config_file; ///< JSON file to load on init (optional)
};

// ─── Core ─────────────────────────────────────────────────────────────────────

class Core {
public:
    using PacketData    = std::shared_ptr<std::vector<uint8_t>>;
    using PacketHandler = std::function<propagation_t(
        std::string_view name,
        std::shared_ptr<header_t> hdr,
        const endpoint_t*         ep,
        PacketData                data)>;

    // ── Lifecycle ─────────────────────────────────────────────────────────────
    explicit Core(CoreConfig cfg = {});
    ~Core();   // Must be out-of-line: Impl is incomplete at header parse time

    Core(const Core&)            = delete;
    Core& operator=(const Core&) = delete;

    /// Block until stop() is called (starts io threads internally).
    void run();
    /// Start io threads in background — non-blocking.
    void run_async(int threads = 0);
    /// Stop all subsystems and join io threads.
    void stop();
    [[nodiscard]] bool is_running() const noexcept;

    // ── Network ───────────────────────────────────────────────────────────────
    void send(const char* uri, uint32_t msg_type, const void* payload, size_t size);
    void send(const char* uri, uint32_t msg_type, std::string_view payload);

    // ── Subscriptions ─────────────────────────────────────────────────────────
    void subscribe(uint32_t msg_type, std::string_view name,
                   PacketHandler cb, uint8_t prio = 128);
    void subscribe_wildcard(std::string_view name,
                            PacketHandler cb, uint8_t prio = 128);

    // ── Identity ──────────────────────────────────────────────────────────────
    [[nodiscard]] std::string user_pubkey_hex()   const;
    [[nodiscard]] std::string device_pubkey_hex() const;

    // ── Stats ─────────────────────────────────────────────────────────────────
    [[nodiscard]] size_t                   connection_count() const noexcept;
    [[nodiscard]] std::vector<std::string> active_uris()      const;

    // ── Internal access for CLI / tests only ─────────────────────────────────
    // Callers must include the concrete headers to use the returned references.
    [[nodiscard]] ConnectionManager& cm()  noexcept;
    [[nodiscard]] PluginManager&     pm()  noexcept;
    [[nodiscard]] SignalBus&         bus() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace gn