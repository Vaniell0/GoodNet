#pragma once
/// @file sdk/types.h
/// @brief GoodNet fundamental wire types, connection identifiers, plugin system.
///
/// Philosophy
/// ──────────
/// This header contains only what is needed to understand wire framing and
/// the plugin ABI.  Protocol version negotiation and capability exchange are
/// application-layer concerns — they are carried inside the AUTH payload and
/// defined in sdk/messages.hpp, not here.
///
/// GN_MAGIC    — frame boundary marker only.
/// GNET_PROTO_VER — framing schema version; changes only when header_t layout
///               changes (extremely rare).

#include <cstdint>
#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

// ── Visibility ────────────────────────────────────────────────────────────────
#if defined(_WIN32)
#   define GN_EXPORT __declspec(dllexport)
#else
#   define GN_EXPORT __attribute__((visibility("default")))
#endif

// ── Wire framing ──────────────────────────────────────────────────────────────
#define GNET_MAGIC      0x474E4554U  ///< ASCII 'GNET' — frame validation only
#define GNET_PROTO_VER  2U           ///< header_t layout version

// ── Header flags (header_t::flags) ──────────────────────────────────────────
#define GNET_FLAG_TRUSTED  0x01U  ///< Plaintext frame (localhost/trusted transport only)

// ── Connection identifier ─────────────────────────────────────────────────────
typedef uint64_t conn_id_t;
#define CONN_ID_INVALID 0ULL

// ── Message type registry ─────────────────────────────────────────────────────
/// Ranges: 0–99 core | 100–999 built-in | 1000–9999 user | 10000+ experimental
#define MSG_TYPE_SYSTEM       0u
#define MSG_TYPE_AUTH         1u
#define MSG_TYPE_KEY_EXCHANGE 2u
#define MSG_TYPE_HEARTBEAT    3u
#define MSG_TYPE_RELAY       10u   ///< Gossip relay (core-level forwarding)
#define MSG_TYPE_ICE_SIGNAL  11u   ///< ICE/DTLS SDP exchange
#define MSG_TYPE_CHAT       100u
#define MSG_TYPE_FILE       200u

// ── Connection lifecycle ──────────────────────────────────────────────────────
typedef enum {
    STATE_CONNECTING,
    STATE_AUTH_PENDING,
    STATE_KEY_EXCHANGE,
    STATE_ESTABLISHED,
    STATE_CLOSING,
    STATE_BLOCKED,
    STATE_CLOSED
} conn_state_t;

// ── Wire header (v2) ──────────────────────────────────────────────────────────
/// Fixed-size frame header preceding every payload on the wire.
///
/// Offset | Field        | Size | Notes
/// -------|--------------|------|----------------------------------------------
///      0 | magic        |    4 | GNET_MAGIC
///      4 | proto_ver    |    1 | GNET_PROTO_VER (2)
///      5 | flags        |    1 | reserved, 0
///      6 | payload_type |    2 | MSG_TYPE_* (uint16_t, max ~100)
///      8 | payload_len  |    4 | bytes following this header
///     12 | packet_id    |    8 | monotonic per-connection counter
///     20 | timestamp    |    8 | sender unix microseconds
///     28 | sender_id    |   16 | first 16 bytes of device_pubkey
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint8_t  proto_ver;
    uint8_t  flags;
    uint16_t payload_type;
    uint32_t payload_len;
    uint64_t packet_id;
    uint64_t timestamp;
    uint8_t  sender_id[16];
} header_t;
#pragma pack(pop)

#ifdef __cplusplus
static_assert(sizeof(header_t) == 44, "header_t must be exactly 44 bytes");
#else
_Static_assert(sizeof(header_t) == 44, "header_t must be exactly 44 bytes");
#endif

/// @brief Remote peer descriptor.
///
/// peer_id is set to the conn_id by the core during dispatch — allows
/// handle_message() to call api->send_response(ep->peer_id, ...) without a
/// separate URI lookup.
typedef struct {
    char     address[128]; ///< NUL-terminated IP or hostname
    uint16_t port;
    uint8_t  pubkey[32];   ///< Peer Ed25519 user pubkey (valid after AUTH)
    uint64_t peer_id;      ///< conn_id — set by core on every dispatch call
    uint8_t  flags;        ///< EP_FLAG_* bitmask, set by connector
} endpoint_t;

/// Endpoint flags (endpoint_t::flags)
#define EP_FLAG_TRUSTED  0x01U  ///< Connector marks connection as trusted (loopback/veth)

// ── Status codes ──────────────────────────────────────────────────────────────
#define STATUS_OK    0
#define STATUS_ERROR 1

// ── Plugin system ─────────────────────────────────────────────────────────────

/// @deprecated Superseded by plugin_info_t::caps_mask.
///             Present so old ABI consumers that read this field don't crash.
typedef enum {
    PLUGIN_TYPE_UNKNOWN   = 0,
    PLUGIN_TYPE_HANDLER   = 1,
    PLUGIN_TYPE_CONNECTOR = 2
} plugin_type_t;

/// @brief PluginManager-managed lifecycle state (hot-reload state machine).
typedef enum {
    PLUGIN_STATE_PREPARING = 0, ///< Loaded; not yet receiving traffic
    PLUGIN_STATE_ACTIVE    = 1, ///< Primary handler for new connections
    PLUGIN_STATE_DRAINING  = 2, ///< Old version; existing sessions still served
    PLUGIN_STATE_ZOMBIE    = 3  ///< Zero active connections; pending dlclose
} plugin_state_t;

/// @brief Packet dispatch chain result.
typedef enum {
    PROPAGATION_CONTINUE = 0, ///< Pass to next handler in priority chain
    PROPAGATION_CONSUMED = 1, ///< Handled; stop chain (pins session affinity)
    PROPAGATION_REJECT   = 2  ///< Invalid; drop silently
} propagation_t;

/// @brief Plugin self-description.
///        Must point to a static object inside the plugin (lifetime = so lifetime).
typedef struct {
    const char* name;      ///< Unique plugin name
    uint32_t    version;   ///< Semantic: (major<<16)|(minor<<8)|patch
    uint8_t     priority;  ///< Dispatch order 0-255 (255 = first)
    uint8_t     _pad[3];
    uint32_t    caps_mask; ///< PLUGIN_CAP_* flags
} plugin_info_t;

/// @name Plugin capability flags (plugin_info_t::caps_mask)
#define PLUGIN_CAP_COMPRESS_ZSTD (1U << 0)
#define PLUGIN_CAP_ICE_SUPPORT   (1U << 1)
#define PLUGIN_CAP_HOT_RELOAD    (1U << 2)

#ifdef __cplusplus
}
#endif