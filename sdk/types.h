#pragma once
/// @file sdk/types.h
/// @brief GoodNet fundamental wire types, connection identifiers, and plugin system.
///
/// This header is the foundation of the GoodNet SDK — included by every SDK
/// header and every plugin.  It defines:
///   - Wire-level framing constants (magic, protocol version)
///   - Connection lifecycle types (`conn_id_t`, `conn_state_t`)
///   - Message type registry (`MSG_TYPE_*`)
///   - Packet header layout (`header_t`)
///   - Remote peer descriptor (`endpoint_t`)
///   - Plugin metadata types (`plugin_info_t`, `propagation_t`)
///
/// @note Crypto size constants (`GN_SIGN_*`) mirror libsodium sizes but this
///       header does NOT depend on sodium.h.  Plugins can safely include this
///       without linking against libsodium.

#include <cstdint>
#include <cstddef>

// ── Crypto key sizes ─────────────────────────────────────────────────────────
/// @name Crypto constants
/// Mirror libsodium Ed25519/X25519 sizes.  Validated at compile time in the
/// core library (cm_capi.cpp) via static_assert against actual sodium values.
/// @{
#define GN_SIGN_PUBLICKEYBYTES 32  ///< Ed25519 public key length (bytes)
#define GN_SIGN_SECRETKEYBYTES 64  ///< Ed25519 secret key length (bytes)
#define GN_SIGN_BYTES          64  ///< Ed25519 signature length (bytes)
#define GN_BOX_PUBLICKEYBYTES  32  ///< X25519 public key length (bytes)
#define GN_BOX_SECRETKEYBYTES  32  ///< X25519 secret key length (bytes)
/// @}

#ifdef __cplusplus
extern "C" {
#endif

// ── Visibility ────────────────────────────────────────────────────────────────
/// @brief Platform-specific symbol export macro.
///
/// Applied to plugin entry points (`handler_init`, `connector_init`,
/// `plugin_get_info`) so the dynamic linker can resolve them.
#if defined(_WIN32)
#   define GN_EXPORT __declspec(dllexport)
#else
#   define GN_EXPORT __attribute__((visibility("default")))
#endif

// ── Wire framing ──────────────────────────────────────────────────────────────
/// @brief Frame boundary marker.  ASCII 'GNET'.
///        Used for stream reassembly validation only — NOT a security boundary.
#define GNET_MAGIC      0x474E4554U

/// @brief Protocol version encoded in every `header_t`.
///        Bumped only when the header_t binary layout changes.
///        Current: v3 (20-byte header).
#define GNET_PROTO_VER  3U

// ── Header flags (header_t::flags) ──────────────────────────────────────────
/// @brief Frame transmitted in plaintext (localhost / trusted transport only).
///        Set by the core for loopback connections where AEAD is bypassed.
#define GNET_FLAG_TRUSTED  0x01U

// ── Connection identifier ─────────────────────────────────────────────────────
/// @brief Opaque, monotonically increasing connection handle.
///        Valid for the lifetime of one TCP/UDP session.  Never reused.
typedef uint64_t conn_id_t;

/// @brief Sentinel value: "no connection".
#define CONN_ID_INVALID 0ULL

// ── Message type registry ─────────────────────────────────────────────────────
/// @name Core message types (0x00–0x0F)
/// Reserved for handshake, heartbeat, and relay — intercepted before user handlers.
/// @{
#define MSG_TYPE_SYSTEM        0u   ///< Reserved (unused)
#define MSG_TYPE_NOISE_INIT    1u   ///< Noise_XX handshake msg1: -> e
#define MSG_TYPE_NOISE_RESP    2u   ///< Noise_XX handshake msg2: <- e, ee, s, es
#define MSG_TYPE_NOISE_FIN     3u   ///< Noise_XX handshake msg3: -> s, se
#define MSG_TYPE_HEARTBEAT     4u   ///< Keepalive ping/pong (see HeartbeatPayload)
#define MSG_TYPE_RELAY        10u   ///< Gossip relay wrapper (see RelayPayload)
#define MSG_TYPE_ICE_SIGNAL   11u   ///< ICE/DTLS SDP exchange (see IceSignalPayload)
/// @}

/// @name User-space message types (>= 100)
/// Free for application / plugin use.  Not intercepted by the core.
/// @{
#define MSG_TYPE_CHAT        100u   ///< Example: chat message
#define MSG_TYPE_FILE        200u   ///< Example: file transfer
/// @}

/// @name System service range (0x0100–0x0FFF)
/// Intercepted before user handlers.  Plugins SHOULD NOT register these types.
/// @{
#define MSG_TYPE_SYS_BASE           0x0100u  ///< First system service type (256)
#define MSG_TYPE_SYS_MAX            0x0FFFu  ///< Last system service type (4095)

// DHT / Service Discovery
#define MSG_TYPE_SYS_DHT_PING       0x0100u  ///< DHT: ping/pong
#define MSG_TYPE_SYS_DHT_FIND_NODE  0x0101u  ///< DHT: find_node request/response
#define MSG_TYPE_SYS_DHT_ANNOUNCE   0x0102u  ///< DHT: announce presence

// Health / Metrics
#define MSG_TYPE_SYS_HEALTH_PING    0x0200u  ///< Health: keepalive ping
#define MSG_TYPE_SYS_HEALTH_PONG    0x0201u  ///< Health: keepalive pong
#define MSG_TYPE_SYS_HEALTH_REPORT  0x0202u  ///< Health: metrics report

// Distributed RPC
#define MSG_TYPE_SYS_RPC_REQUEST    0x0300u  ///< RPC: request
#define MSG_TYPE_SYS_RPC_RESPONSE   0x0301u  ///< RPC: response

// Routing
#define MSG_TYPE_SYS_ROUTE_ANNOUNCE 0x0400u  ///< Routing: announce known routes
#define MSG_TYPE_SYS_ROUTE_QUERY    0x0401u  ///< Routing: query path to pubkey

// TUN/TAP
#define MSG_TYPE_SYS_TUN_CONFIG     0x0500u  ///< TUN/TAP: tunnel configuration
#define MSG_TYPE_SYS_TUN_DATA       0x0501u  ///< TUN/TAP: encapsulated IP packet

// Store / Distributed Registry
#define MSG_TYPE_SYS_STORE_PUT       0x0600u  ///< Store: put key-value entry
#define MSG_TYPE_SYS_STORE_GET       0x0601u  ///< Store: get by key
#define MSG_TYPE_SYS_STORE_RESULT    0x0602u  ///< Store: query result
#define MSG_TYPE_SYS_STORE_DELETE    0x0603u  ///< Store: delete entry
#define MSG_TYPE_SYS_STORE_SUBSCRIBE 0x0604u  ///< Store: watch key for changes
#define MSG_TYPE_SYS_STORE_NOTIFY    0x0605u  ///< Store: change notification
#define MSG_TYPE_SYS_STORE_SYNC      0x0606u  ///< Store: bulk sync between stores
/// @}

// ── Connection lifecycle ──────────────────────────────────────────────────────
/// @brief Connection state machine.
///
/// State flow:
///   CONNECTING -> NOISE_HANDSHAKE -> ESTABLISHED -> CLOSING -> CLOSED
///
/// Plugins receive `handle_conn_state()` callbacks on every transition.
typedef enum {
    STATE_CONNECTING,       ///< TCP connected, Noise not yet started
    STATE_NOISE_HANDSHAKE,  ///< Noise_XX handshake in progress (3-message)
    STATE_ESTABLISHED,      ///< Handshake complete — transport encryption active
    STATE_CLOSING,          ///< Graceful close initiated (send queue draining)
    STATE_CLOSED            ///< Connection fully closed and resources released
} conn_state_t;

// ── Wire header (v3) ──────────────────────────────────────────────────────────
/// @brief 20-byte wire frame header.
///
/// Every GoodNet frame starts with this header, followed by `payload_len`
/// bytes of (possibly AEAD-encrypted) payload.
///
/// The `packet_id` field serves dual purpose:
///   1. Monotonic counter for replay detection (NonceWindow)
///   2. AEAD nonce for ChaChaPoly-IETF encryption
///
/// @verbatim
/// Offset | Field        | Size | Notes
/// -------|--------------|------|----------------------------------------------
///      0 | magic        |    4 | GNET_MAGIC (0x474E4554)
///      4 | proto_ver    |    1 | GNET_PROTO_VER (3)
///      5 | flags        |    1 | GNET_FLAG_* bitmask
///      6 | payload_type |    2 | MSG_TYPE_* (little-endian)
///      8 | payload_len  |    4 | Payload byte count (little-endian)
///     12 | packet_id    |    8 | Monotonic nonce (little-endian)
/// @endverbatim

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;        ///< Must equal GNET_MAGIC
    uint8_t  proto_ver;    ///< Must equal GNET_PROTO_VER
    uint8_t  flags;        ///< GNET_FLAG_* bitmask
    uint16_t payload_type; ///< MSG_TYPE_*
    uint32_t payload_len;  ///< Byte count of payload following this header
    uint64_t packet_id;    ///< Monotonic counter / AEAD nonce
} header_t;
#pragma pack(pop)

#ifdef __cplusplus
static_assert(sizeof(header_t) == 20, "header_t must be exactly 20 bytes");
#else
_Static_assert(sizeof(header_t) == 20, "header_t must be exactly 20 bytes");
#endif

/// @brief Remote peer descriptor.
///
/// Filled by the connector plugin on accept/connect, then enriched by the core
/// with `peer_id` on every dispatch call.
///
/// Thread-safety: read-only after the core assigns `peer_id`.  The connector
/// owns the address/port fields; the core fills pubkey after handshake.
typedef struct {
    char     address[128]; ///< NUL-terminated IP or hostname
    uint16_t port;         ///< Peer port (network order in connector, host order here)
    uint8_t  pubkey[GN_SIGN_PUBLICKEYBYTES]; ///< Peer Ed25519 user pubkey (valid after ESTABLISHED)
    uint64_t peer_id;      ///< conn_id assigned by core on on_connect() — use for send_response()
    uint8_t  flags;        ///< EP_FLAG_* bitmask, set by connector
} endpoint_t;

/// @name Endpoint flags (endpoint_t::flags)
/// @{
#define EP_FLAG_TRUSTED   0x01U  ///< Connector marks connection as trusted (loopback/veth)
#define EP_FLAG_OUTBOUND  0x02U  ///< Outgoing connection — initiator in Noise_XX handshake
/// @}

// ── Status codes ──────────────────────────────────────────────────────────────
#define STATUS_OK    0  ///< Operation succeeded
#define STATUS_ERROR 1  ///< Operation failed (generic)

// ── Plugin system ─────────────────────────────────────────────────────────────

/// @brief Plugin lifecycle state (managed by PluginManager).
///
/// Transitions: PREPARING -> ACTIVE -> DRAINING -> ZOMBIE
typedef enum {
    PLUGIN_STATE_PREPARING = 0, ///< Plugin loaded, init() not yet called
    PLUGIN_STATE_ACTIVE    = 1, ///< Plugin initialized and receiving events
    PLUGIN_STATE_DRAINING  = 2, ///< shutdown() called, waiting for in-flight events
    PLUGIN_STATE_ZOMBIE    = 3  ///< dlclose() imminent — no callbacks will fire
} plugin_state_t;

/// @brief Handler chain-of-responsibility result.
///
/// Returned by `handler_t::on_message_result()` to control dispatch pipeline:
///   - CONTINUE: pass packet to next handler in priority order
///   - CONSUMED: stop dispatch; pin session affinity to this handler
///   - REJECT:   stop dispatch; silently drop packet (no further handlers)
typedef enum {
    PROPAGATION_CONTINUE = 0, ///< Pass to next handler
    PROPAGATION_CONSUMED = 1, ///< Handler claimed the packet (stops chain)
    PROPAGATION_REJECT   = 2  ///< Drop packet silently (stops chain)
} propagation_t;

/// @brief Plugin metadata descriptor.
///
/// Exported by `plugin_get_info()` or embedded in `handler_t::info`.
/// Read by PluginManager for logging, dependency resolution, and cap negotiation.
typedef struct {
    const char* name;      ///< Human-readable plugin name (static string)
    uint32_t    version;   ///< Packed version: (major<<16) | (minor<<8) | patch
    uint8_t     priority;  ///< Dispatch priority (0=highest, 255=lowest). Default: 128.
    uint8_t     _pad[3];   ///< Reserved (zero-fill)
    uint32_t    caps_mask; ///< PLUGIN_CAP_* feature flags
} plugin_info_t;

/// @name Plugin capability flags (plugin_info_t::caps_mask)
/// @{
#define PLUGIN_CAP_COMPRESS_ZSTD (1U << 0) ///< Plugin supports zstd payload compression
#define PLUGIN_CAP_ICE_SUPPORT   (1U << 1) ///< Plugin provides ICE/DTLS transport
#define PLUGIN_CAP_HOT_RELOAD    (1U << 2) ///< Plugin supports hot-reload without restart
/// @}

#ifdef __cplusplus
}
#endif
