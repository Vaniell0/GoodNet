#pragma once
#include <cstdint>
#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

/// @defgroup types GoodNet Core Types
/// @brief Fundamental wire types, identifiers and constants.
/// @{

/// @brief Ensures symbol visibility through dlsym with -fvisibility=hidden.
#if defined(_WIN32)
#   define GN_EXPORT __declspec(dllexport)
#else
#   define GN_EXPORT __attribute__((visibility("default")))
#endif

/// @name Protocol constants
/// @{
#define GNET_MAGIC     0x474E4554U  ///< Wire magic: ASCII 'GNET'
#define GNET_PROTO_VER 1U           ///< Current protocol version
/// @}

/// @brief Opaque connection identifier. 0 = CONN_ID_INVALID.
typedef uint64_t conn_id_t;
#define CONN_ID_INVALID 0ULL        ///< Returned on connection failure

/// @brief Plugin role, set by PluginManager before calling *_init().
typedef enum {
    PLUGIN_TYPE_UNKNOWN   = 0,
    PLUGIN_TYPE_HANDLER   = 1,
    PLUGIN_TYPE_CONNECTOR = 2
} plugin_type_t;

/// @name Message types
/// @brief Ranges: 0–99 reserved for core, 100–999 for built-in plugins,
///        1000–9999 for user plugins, 10000+ experimental.
/// @{
#define MSG_TYPE_SYSTEM       0u   ///< Internal core messages
#define MSG_TYPE_AUTH         1u   ///< Handshake (plaintext, pre-session)
#define MSG_TYPE_KEY_EXCHANGE 2u   ///< Key exchange (reserved)
#define MSG_TYPE_HEARTBEAT    3u   ///< Keepalive ping/pong
#define MSG_TYPE_CHAT       100u   ///< Text messages
#define MSG_TYPE_FILE       200u   ///< File transfer
/// @}

/// @brief Connection lifecycle state machine.
typedef enum {
    STATE_CONNECTING,   ///< TCP connect in progress
    STATE_AUTH_PENDING, ///< Waiting for peer AUTH
    STATE_KEY_EXCHANGE, ///< ECDH in progress (reserved)
    STATE_ESTABLISHED,  ///< Authenticated, session key ready
    STATE_CLOSING,      ///< Graceful close initiated
    STATE_BLOCKED,      ///< Blocked by policy
    STATE_CLOSED        ///< Connection terminated
} conn_state_t;

/// @brief Fixed-size packet header. Precedes every payload on the wire.
///
/// Layout (packed, no padding):
/// ```
/// [0]  magic(4) proto_ver(1) flags(1) reserved(2)
/// [8]  packet_id(8)
/// [16] timestamp(8)  — unix microseconds
/// [24] payload_type(4) status(2) payload_len(4)
/// [34] signature(64) — Ed25519(device_sk, header[0..33]); zero until ESTABLISHED
/// ```
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;         ///< Must equal GNET_MAGIC
    uint8_t  proto_ver;     ///< Must equal GNET_PROTO_VER
    uint8_t  flags;         ///< Reserved, must be 0
    uint16_t reserved;      ///< Reserved, must be 0
    uint64_t packet_id;     ///< Monotonic per-connection counter
    uint64_t timestamp;     ///< Send time, unix microseconds
    uint32_t payload_type;  ///< MSG_TYPE_* value
    uint16_t status;        ///< STATUS_OK or STATUS_ERROR
    uint32_t payload_len;   ///< Byte length of payload following this header
    uint8_t  signature[64]; ///< Ed25519 signature; zero bytes until ESTABLISHED
} header_t;
#pragma pack(pop)

/// @brief Remote peer address descriptor.
typedef struct {
    char     address[128]; ///< IP or hostname, NUL-terminated
    uint16_t port;
    uint8_t  pubkey[32];   ///< Peer Ed25519 user pubkey (populated after AUTH)
    uint64_t peer_id;      ///< Reserved
} endpoint_t;

/// @name Status codes
/// @{
#define STATUS_OK    0
#define STATUS_ERROR 1
/// @}

/// @}  // defgroup types

#ifdef __cplusplus
}
#endif
