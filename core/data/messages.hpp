#pragma once
/// @file sdk/messages.hpp
/// @brief GoodNet typed wire payloads.
///
/// This is the application-layer protocol definition.  Every MSG_TYPE_*
/// constant has a corresponding packed struct here.
///
/// Rules
/// ─────
///   • All structs use #pragma pack(push,1) — guarantees wire compatibility.
///   • PodData<T>  — zero-copy wrapper for fixed-layout payloads.
///   • New fields must always be appended at the end of a payload struct.

#include "../sdk/cpp/data.hpp"   // PodData<T>, IData

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

// Uses GN_SIGN_PUBLICKEYBYTES etc. from types.h (no sodium dependency in SDK)

// GN_CORE_VERSION is now GOODNET_VERSION_PACKED from include/version.hpp.
// Kept as alias for backward-compat in handshake payloads.
#include "version.hpp"
#define GN_CORE_VERSION GOODNET_VERSION_PACKED

namespace gn::msg {

// ─── Core capability metadata ─────────────────────────────────────────────────
/// Exchanged inside AuthPayload so both peers know what features the other
/// supports.  This is NOT in types.h because it is payload data, not framing.

/// @brief Feature flags advertised by a running GoodNet core.
#define CORE_CAP_ZSTD   (1U << 0) ///< Payload compression available
#define CORE_CAP_ICE    (1U << 1) ///< ICE/DTLS transport supported
#define CORE_CAP_KEYROT (1U << 2) ///< On-line key rotation supported
#define CORE_CAP_RELAY  (1U << 3) ///< Gossip relay supported

#pragma pack(push, 1)
/// @brief Peer capability block embedded in AuthPayload.
///        Zero-filled by old clients — receiver treats all-zero as "unknown".
struct CoreMeta {
    uint32_t core_version; ///< GN_CORE_VERSION of the sender
    uint32_t caps_mask;    ///< CORE_CAP_* flags
};
#pragma pack(pop)

// ─── Noise handshake payload (msg2/msg3) ─────────────────────────────────────
/// Embedded in the encrypted portion of Noise_XX msg2 (responder) and
/// msg3 (initiator). Carries identity info + capability negotiation.
///
/// Note: the Noise static key (X25519, derived from device_key) is transmitted
/// by the Noise protocol itself (the `s` token). This payload adds the Ed25519
/// identity needed for user-level authentication.

static constexpr uint8_t HANDSHAKE_MAX_SCHEMES = 8;
static constexpr uint8_t HANDSHAKE_SCHEME_LEN  = 16;

#pragma pack(push, 1)
struct HandshakePayload {
    uint8_t user_pubkey  [GN_SIGN_PUBLICKEYBYTES]; ///< Ed25519 portable identity
    uint8_t device_pubkey[GN_SIGN_PUBLICKEYBYTES]; ///< Ed25519 device key (for verification)
    uint8_t signature    [GN_SIGN_BYTES];           ///< Ed25519(user_sk, user_pk||device_pk)

    uint8_t schemes_count;
    char    schemes[HANDSHAKE_MAX_SCHEMES][HANDSHAKE_SCHEME_LEN];

    CoreMeta core_meta;

    // ── Helpers ───────────────────────────────────────────────────────────────
    void set_schemes(const std::vector<std::string>& sv) {
        schemes_count = static_cast<uint8_t>(
            std::min(sv.size(), static_cast<size_t>(HANDSHAKE_MAX_SCHEMES)));
        for (uint8_t i = 0; i < schemes_count; ++i) {
            std::strncpy(schemes[i], sv[i].c_str(), HANDSHAKE_SCHEME_LEN - 1);
            schemes[i][HANDSHAKE_SCHEME_LEN - 1] = '\0';
        }
        for (uint8_t i = schemes_count; i < HANDSHAKE_MAX_SCHEMES; ++i)
            schemes[i][0] = '\0';
    }

    std::vector<std::string> get_schemes() const {
        std::vector<std::string> out;
        out.reserve(schemes_count);
        for (uint8_t i = 0; i < schemes_count; ++i)
            out.emplace_back(schemes[i]);
        return out;
    }
};
#pragma pack(pop)

static constexpr size_t kHandshakePayloadSize = sizeof(HandshakePayload);

// ─── HEARTBEAT (MSG_TYPE_HEARTBEAT = 4) ───────────────────────────────────────
/// Keepalive ping/pong.  flags 0x00 = ping, 0x01 = pong.

#pragma pack(push, 1)
struct HeartbeatPayload {
    uint64_t timestamp_us; ///< Sender unix microseconds
    uint32_t seq;          ///< Monotonic counter (used to compute RTT)
    uint8_t  flags;        ///< 0x00 = ping, 0x01 = pong
    uint8_t  _pad[3];
};
#pragma pack(pop)
static_assert(sizeof(HeartbeatPayload) == 16, "HeartbeatPayload size mismatch");

using HeartbeatMessage = gn::sdk::PodData<HeartbeatPayload>;

/// Backward-compatible heartbeat extension: transport path status.
/// Appended after base HeartbeatPayload (16 bytes). Old peers ignore extra bytes.
static constexpr uint8_t HEARTBEAT_MAX_PATHS = 8;

#pragma pack(push, 1)
struct HeartbeatTransportInfo {
    uint8_t path_count;  ///< Number of HeartbeatPathEntry following
};
struct HeartbeatPathEntry {
    char     scheme[16];       ///< Transport scheme name (null-terminated)
    uint8_t  active;           ///< 1 = active, 0 = inactive
    uint8_t  priority;         ///< Lower = better
    uint16_t rtt_compressed;   ///< RTT in units of 10us (max 655350us ≈ 0.65s)
};
#pragma pack(pop)
static_assert(sizeof(HeartbeatPathEntry) == 20, "HeartbeatPathEntry size mismatch");

// ─── RELAY (MSG_TYPE_RELAY = 10) ───────────────────────────────────────────────
/// Gossip relay wrapper. Encrypted payload after decrypt:
///   ttl(1) | dest_pubkey(32) | inner_frame (header_t + encrypted payload)
/// inner_frame is opaque to relay nodes — forwarded as-is.

#pragma pack(push, 1)
struct RelayPayload {
    uint8_t  ttl;              ///< Remaining hops (decremented, drop at 0)
    uint8_t  dest_pubkey[GN_SIGN_PUBLICKEYBYTES];  ///< Final destination user_pubkey
    // Followed by inner_frame: original header_t + encrypted payload (opaque)
};
#pragma pack(pop)
static_assert(sizeof(RelayPayload) == 33, "RelayPayload size mismatch");

// ─── ICE_SIGNAL (MSG_TYPE_ICE_SIGNAL = 11) ────────────────────────────────────
/// Carries SDP blobs between peers over the existing TCP signaling channel.
/// Sent by the ICE connector plugin; routed through the normal packet pipeline.

enum class IceSignalKind : uint8_t {
    OFFER  = 0, ///< Initiating peer sends local SDP + candidates
    ANSWER = 1, ///< Responding peer echoes its local SDP
};

#pragma pack(push, 1)
struct IceSignalPayload {
    uint8_t  kind;    ///< IceSignalKind
    uint8_t  _pad[3];
    uint32_t sdp_len; ///< Byte count of the UTF-8 SDP string that follows
    // Followed immediately by sdp_len bytes of SDP text (variable length).
};
#pragma pack(pop)

// ─── DHT / Service Discovery ─────────────────────────────────────────────────

#pragma pack(push, 1)
struct DhtPingPayload {
    uint64_t request_id;        ///< Unique request identifier
    uint8_t  sender_pubkey[GN_SIGN_PUBLICKEYBYTES]; ///< Sender's user pubkey
    uint16_t listen_port;       ///< Sender's listening port
    uint8_t  _pad[2];
};
#pragma pack(pop)
static_assert(sizeof(DhtPingPayload) == 44, "DhtPingPayload size mismatch");

#pragma pack(push, 1)
struct DhtFindNodePayload {
    uint64_t request_id;
    uint8_t  target_pubkey[GN_SIGN_PUBLICKEYBYTES]; ///< Key we're looking for
    uint8_t  k;                 ///< Number of closest nodes requested
    uint8_t  response_count;    ///< 0 = request, >0 = response with entries
    uint8_t  _pad[2];
    // Followed by response_count * DhtNodeEntry (variable)
};
#pragma pack(pop)

#pragma pack(push, 1)
struct DhtNodeEntry {
    uint8_t  pubkey[GN_SIGN_PUBLICKEYBYTES];
    char     address[64];  ///< ip:port
    uint16_t port;
    uint8_t  _pad[2];
};
#pragma pack(pop)

#pragma pack(push, 1)
struct DhtAnnouncePayload {
    uint8_t  pubkey[GN_SIGN_PUBLICKEYBYTES];
    char     address[64];
    uint16_t port;
    uint8_t  flags;        ///< 0x01=leaving
    uint8_t  _pad;
};
#pragma pack(pop)

// ─── Health / Metrics ─────────────────────────────────────────────────────────

#pragma pack(push, 1)
struct HealthPingPayload {
    uint64_t timestamp_us;      ///< Sender unix microseconds
    uint32_t seq;               ///< Monotonic counter
    uint32_t connection_count;  ///< Sender's active connections
};
#pragma pack(pop)
static_assert(sizeof(HealthPingPayload) == 16, "HealthPingPayload size mismatch");

#pragma pack(push, 1)
struct HealthReportPayload {
    uint64_t uptime_s;
    uint32_t connections;
    uint32_t rx_bytes_sec;
    uint32_t tx_bytes_sec;
    uint16_t cpu_percent;    ///< x100 (5000 = 50.00%)
    uint16_t mem_mb;
    uint32_t latency_p50_us;
    uint32_t latency_p99_us;
};
#pragma pack(pop)
static_assert(sizeof(HealthReportPayload) == 32, "HealthReportPayload size mismatch");

// ─── Distributed RPC ─────────────────────────────────────────────────────────

/// Compile-time FNV-1a hash for RPC method names.
/// Usage: send_rpc(GN_RPC_HASH("get_status"), payload, len);
constexpr uint32_t gn_rpc_hash(const char* s, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; ++i)
        h = (h ^ static_cast<uint32_t>(s[i])) * 16777619u;
    return h;
}
#define GN_RPC_HASH(str) (::gn::msg::gn_rpc_hash(str, sizeof(str) - 1))

#pragma pack(push, 1)
struct RpcRequestPayload {
    uint64_t request_id;
    uint32_t method_hash;    ///< GN_RPC_HASH("method_name") — compile-time FNV-1a
    uint32_t timeout_ms;
    // Followed by payload bytes (variable)
};
#pragma pack(pop)
static_assert(sizeof(RpcRequestPayload) == 16, "RpcRequestPayload size mismatch");

#pragma pack(push, 1)
struct RpcResponsePayload {
    uint64_t request_id;     ///< Matches RpcRequestPayload::request_id
    uint32_t status;         ///< 0=OK, 1=ERROR, 2=TIMEOUT, 3=NOT_FOUND
    uint32_t _reserved;
    // Followed by payload bytes (variable)
};
#pragma pack(pop)
static_assert(sizeof(RpcResponsePayload) == 16, "RpcResponsePayload size mismatch");

// ─── Routing ──────────────────────────────────────────────────────────────────

#pragma pack(push, 1)
struct RouteAnnouncePayload {
    uint8_t  dest_pubkey[GN_SIGN_PUBLICKEYBYTES];  ///< Reachable destination
    uint8_t  via_pubkey[GN_SIGN_PUBLICKEYBYTES];   ///< Next hop (announcer)
    uint8_t  hop_count;        ///< Distance metric
    uint8_t  flags;            ///< 0x01=withdraw
    uint16_t ttl_sec;          ///< Entry validity time
    uint32_t seq_num;          ///< Monotonic for conflict resolution
};
#pragma pack(pop)
static_assert(sizeof(RouteAnnouncePayload) == 72, "RouteAnnouncePayload size mismatch");

#pragma pack(push, 1)
struct RouteQueryPayload {
    uint64_t request_id;
    uint8_t  target_pubkey[GN_SIGN_PUBLICKEYBYTES];
    uint8_t  is_response;      ///< 0=query, 1=response
    uint8_t  hop_count;        ///< Hops from responder to target
    uint8_t  _pad[2];
    // If is_response: followed by via_pubkey[GN_SIGN_PUBLICKEYBYTES]
};
#pragma pack(pop)

// ─── TUN/TAP ──────────────────────────────────────────────────────────────────

#pragma pack(push, 1)
struct TunConfigPayload {
    uint32_t tunnel_id;
    uint8_t  virtual_ip[4];     ///< Assigned virtual IPv4
    uint8_t  netmask[4];        ///< /24 default
    uint8_t  gateway_pubkey[GN_SIGN_PUBLICKEYBYTES]; ///< Mesh gateway
    uint16_t mtu;
    uint8_t  flags;             ///< 0x01=IPv6 support
    uint8_t  _pad;
};
#pragma pack(pop)
static_assert(sizeof(TunConfigPayload) == 48, "TunConfigPayload size mismatch");

#pragma pack(push, 1)
struct TunDataPayload {
    uint32_t tunnel_id;
    uint16_t ip_len;            ///< Length of encapsulated IP packet (MUST <= MTU)
    uint8_t  _pad[2];
    // Followed by ip_len bytes of raw IP packet
    //
    // IMPORTANT: TUN/TAP plugin MUST validate ip_len <= TunConfigPayload::mtu
    // before encapsulation. Packets exceeding MTU are dropped with
    // DropReason::TunMtuExceeded — no kernel-level fragmentation allowed.
};
#pragma pack(pop)
static_assert(sizeof(TunDataPayload) == 8, "TunDataPayload size mismatch");

// ─── Store / Distributed Registry ───────────────────────────────────────────
/// Generic key-value store protocol. Keys use namespace-based convention:
///   - "peer/<pubkey_hex>" — peer directory
///   - "service/<name>"    — service registry
///   - "route/<dest_hex>"  — routing table
/// Variable-length fields follow fixed headers (key_len, value_len).

static constexpr uint16_t STORE_KEY_MAX_LEN   = 128;
static constexpr uint16_t STORE_VALUE_MAX_LEN = 4096;

#pragma pack(push, 1)

/// MSG_TYPE_SYS_STORE_PUT (0x0600)
struct StorePutPayload {
    uint64_t request_id;
    uint64_t ttl_s;          ///< Time-to-live in seconds (0 = permanent)
    uint16_t key_len;        ///< Actual key length (≤ STORE_KEY_MAX_LEN)
    uint16_t value_len;      ///< Actual value length (≤ STORE_VALUE_MAX_LEN)
    uint8_t  flags;          ///< 0x01 = replicate, 0x02 = overwrite_only
    uint8_t  _pad[3];
    // Followed by: key_len bytes of key + value_len bytes of value
};
static_assert(sizeof(StorePutPayload) == 24, "StorePutPayload size mismatch");

/// MSG_TYPE_SYS_STORE_GET (0x0601)
struct StoreGetPayload {
    uint64_t request_id;
    uint16_t key_len;
    uint8_t  query_type;     ///< 0 = exact, 1 = prefix, 2 = list namespace
    uint8_t  _pad;
    uint32_t max_results;    ///< 0 = default (32)
    // Followed by: key_len bytes of key (or prefix)
};
static_assert(sizeof(StoreGetPayload) == 16, "StoreGetPayload size mismatch");

/// MSG_TYPE_SYS_STORE_RESULT (0x0602)
struct StoreResultPayload {
    uint64_t request_id;
    uint8_t  entry_count;
    uint8_t  status;         ///< 0 = ok, 1 = not_found, 2 = error
    uint8_t  _pad[2];
    uint32_t total_count;    ///< Total matches (for pagination)
    // Followed by: entry_count * StoreEntry (each with trailing key + value)
};
static_assert(sizeof(StoreResultPayload) == 16, "StoreResultPayload size mismatch");

/// Single entry in a RESULT or SYNC payload.
struct StoreEntry {
    uint16_t key_len;
    uint16_t value_len;
    uint64_t timestamp_us;   ///< When this entry was last updated
    uint64_t ttl_s;          ///< Remaining TTL (0 = permanent)
    uint8_t  flags;
    uint8_t  _pad[3];
    // Followed by: key_len bytes of key + value_len bytes of value
};
static_assert(sizeof(StoreEntry) == 24, "StoreEntry size mismatch");

/// MSG_TYPE_SYS_STORE_DELETE (0x0603)
struct StoreDeletePayload {
    uint64_t request_id;
    uint16_t key_len;
    uint8_t  _pad[6];
    // Followed by: key_len bytes of key
};
static_assert(sizeof(StoreDeletePayload) == 16, "StoreDeletePayload size mismatch");

/// MSG_TYPE_SYS_STORE_SUBSCRIBE (0x0604)
struct StoreSubscribePayload {
    uint64_t request_id;
    uint16_t key_len;
    uint8_t  action;         ///< 0 = subscribe, 1 = unsubscribe
    uint8_t  query_type;     ///< 0 = exact, 1 = prefix
    uint8_t  _pad[4];
    // Followed by: key_len bytes of key/prefix
};
static_assert(sizeof(StoreSubscribePayload) == 16, "StoreSubscribePayload size mismatch");

/// MSG_TYPE_SYS_STORE_NOTIFY (0x0605)
struct StoreNotifyPayload {
    uint8_t  event;          ///< 0 = created, 1 = updated, 2 = deleted, 3 = expired
    uint8_t  _pad[3];
    uint64_t timestamp_us;
    // Followed by: StoreEntry (key + value)
};
static_assert(sizeof(StoreNotifyPayload) == 12, "StoreNotifyPayload size mismatch");

/// MSG_TYPE_SYS_STORE_SYNC (0x0606)
struct StoreSyncPayload {
    uint64_t request_id;
    uint8_t  sync_type;      ///< 0 = full_request, 1 = full_response, 2 = delta
    uint8_t  entry_count;
    uint8_t  _pad[2];
    uint64_t since_timestamp; ///< For delta: entries newer than this
    // Followed by: entry_count * StoreEntry (each with trailing key + value)
};
static_assert(sizeof(StoreSyncPayload) == 20, "StoreSyncPayload size mismatch");

#pragma pack(pop)

} // namespace gn::msg
