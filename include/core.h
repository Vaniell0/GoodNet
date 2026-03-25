#ifndef GOODNET_H
#define GOODNET_H
/// @file include/core.h
/// @brief GoodNet C ABI — stable FFI interface for non-C++ consumers.
///
/// This header exposes the full GoodNet core functionality through a flat C API.
/// Suitable for FFI bindings (Python, Rust, Go, etc.) and C-only projects.
///
/// ## Lifecycle
///   1. `gn_core_create(cfg)`   — allocate and configure a core instance
///   2. `gn_core_run_async(core, threads)` — start IO threads
///   3. Use send/broadcast/subscribe as needed
///   4. `gn_core_stop(core)`    — stop IO threads
///   5. `gn_core_destroy(core)` — release all resources
///
/// ## Thread-safety
///   All functions are thread-safe after `gn_core_create()` returns.

#include <stdint.h>
#include <stddef.h>
#include "../sdk/types.h"   /* propagation_t, conn_id_t */

/// @brief Number of DropReason variants.  Must match `DropReason::_Count` in signals.hpp.
#define GN_DROP_REASON_COUNT 17

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Opaque core handle.  Created by gn_core_create(), freed by gn_core_destroy().
typedef struct gn_core_t gn_core_t;

/// @brief Configuration passed to gn_core_create().
///
/// All fields are optional — NULL/0 values use defaults.
typedef struct {
    const char* config_dir;   ///< Identity key directory (default: "~/.goodnet")
    const char* log_level;    ///< Log level string: "trace","debug","info","warn","error","off"
    uint16_t    listen_port;  ///< TCP listen port (default: 25565)
} gn_config_t;

/// @brief Atomic statistics snapshot.
///
/// Returned by gn_core_get_stats().  All counters are monotonically increasing
/// (except `connections` which is a gauge).
typedef struct {
    uint64_t rx_bytes, tx_bytes;           ///< Total bytes received/sent
    uint64_t rx_packets, tx_packets;       ///< Total packets received/sent
    uint64_t auth_ok, auth_fail;           ///< Handshake success/failure count
    uint64_t decrypt_fail, backpressure;   ///< AEAD failures, send-queue backpressure events
    uint64_t consumed, rejected;           ///< Handler chain: consumed/rejected packet count
    uint32_t connections, total_conn, total_disc; ///< Active/total-connected/total-disconnected
    uint64_t drops[GN_DROP_REASON_COUNT];  ///< Per-reason drop counters (indexed by DropReason)
    uint64_t dispatch_lat_avg;             ///< Average dispatch latency (nanoseconds)
} gn_stats_t;

// ── Lifecycle ─────────────────────────────────────────────────────────────────

/// @brief Create a new core instance.
/// @param cfg  Configuration (may be NULL for defaults).
/// @return Opaque handle, or NULL on allocation failure.
gn_core_t* gn_core_create    (gn_config_t* cfg);

/// @brief Destroy a core instance and release all resources.
/// @param core  Handle from gn_core_create().  NULL is a no-op.
void       gn_core_destroy   (gn_core_t* core);

/// @brief Run the IO loop on the calling thread (blocks until gn_core_stop()).
void       gn_core_run       (gn_core_t* core);

/// @brief Start IO threads in the background (non-blocking).
/// @param threads  Number of threads.  0 = auto (hardware concurrency).
void       gn_core_run_async (gn_core_t* core, int threads);

/// @brief Stop all IO threads and unload plugins.
void       gn_core_stop      (gn_core_t* core);

/// @brief Hot-reload configuration from disk.
/// @return 0 on success, -1 on error.
int        gn_core_reload_config(gn_core_t* core);

/// @brief Check whether the core is currently running.
/// @return 1 if running, 0 if stopped or NULL.
int        gn_core_is_running(gn_core_t* core);

// ── Network ───────────────────────────────────────────────────────────────────

/// @brief Send a packet to a peer by URI.
/// @param uri   Peer address (e.g. "tcp://host:port" or hex pubkey).
/// @param type  Wire message type (MSG_TYPE_*).
/// @param data  Payload bytes.
/// @param len   Payload byte count.
void gn_core_send      (gn_core_t* core, const char* uri, uint32_t type,
                         const void* data, size_t len);

/// @brief Broadcast to all ESTABLISHED peers.
void gn_core_broadcast (gn_core_t* core, uint32_t type,
                         const void* data, size_t len);

/// @brief Disconnect a peer by connection ID.
void gn_core_disconnect(gn_core_t* core, uint64_t conn_id);

/// @brief Re-derive transport keys for a connection (Noise native rekey).
/// @return 0 on success, -1 if not found or not ESTABLISHED.
int  gn_core_rekey     (gn_core_t* core, uint64_t conn_id);

// ── Identity ──────────────────────────────────────────────────────────────────

/// @brief Get the node's hex-encoded Ed25519 user public key.
/// @param buffer   Output buffer.
/// @param max_len  Buffer capacity.
/// @return Number of bytes written (excluding NUL), 0 on error.
size_t gn_core_get_user_pubkey(gn_core_t* core, char* buffer, size_t max_len);

// ── Stats ─────────────────────────────────────────────────────────────────────

/// @brief Fill a statistics snapshot.
/// @param out  Output struct (caller-owned).
void gn_core_get_stats(gn_core_t* core, gn_stats_t* out);

/// @brief Get the number of currently active connections.
/// @return Connection count, or 0 if core is NULL.
uint32_t gn_core_connection_count(gn_core_t* core);

// ── Diagnostics ──────────────────────────────────────────────────────────────

/// @brief JSON dump of all active connections (diagnostic).
/// @param buf      Output buffer.
/// @param max_len  Buffer capacity.
/// @return Number of bytes written (excluding NUL), 0 on error or if buf too small.
size_t gn_core_dump_connections(gn_core_t* core, char* buf, size_t max_len);

/// @brief Get number of registered handler plugins.
uint32_t gn_core_handler_count(gn_core_t* core);

/// @brief Get number of registered connector plugins.
uint32_t gn_core_connector_count(gn_core_t* core);

// ── Version ───────────────────────────────────────────────────────────────────

/// @brief Get the library version string (e.g. "0.1.0-alpha").
/// @return Static string — do not free.
const char* gn_version(void);

// ── Subscriptions ─────────────────────────────────────────────────────────────

/// @brief C callback for message subscriptions.
/// @param type       Wire message type.
/// @param data       Decrypted payload.
/// @param len        Payload byte count.
/// @param user_data  User context from gn_core_subscribe().
/// @return Propagation decision (CONTINUE/CONSUMED/REJECT).
typedef propagation_t (*gn_handler_fn)(uint32_t type, const void* data,
                                        size_t len, void* user_data);

/// @brief Subscribe to a message type.
/// @param type       Wire message type to listen for.
/// @param cb         Callback function.
/// @param user_data  Opaque context passed to cb.
/// @return Subscription token for gn_core_unsubscribe(), or 0 on error.
uint64_t gn_core_subscribe  (gn_core_t* core, uint32_t type,
                               gn_handler_fn cb, void* user_data);

/// @brief Remove a subscription.
/// @param sub_id  Token from gn_core_subscribe().
void     gn_core_unsubscribe(gn_core_t* core, uint64_t sub_id);

#ifdef __cplusplus
}
#endif

#endif // GOODNET_H
