#pragma once
/// @file sdk/messages.hpp
/// @brief GoodNet typed wire payloads.
///
/// This is the application-layer protocol definition.  Every MSG_TYPE_*
/// constant has a corresponding packed struct here.  Version negotiation and
/// capability exchange are carried inside AuthPayload — not in types.h —
/// because they are negotiated at connection time, not baked into the ABI.
///
/// Rules
/// ─────
///   • All structs use #pragma pack(push,1) — guarantees wire compatibility.
///   • PodData<T>  — zero-copy wrapper for fixed-layout payloads.
///   • New fields must always be appended at the end of a payload struct.
///   • kBaseSize / kFullSize constants allow old implementations to send
///     smaller payloads that new code still accepts (forward compat).

#include "../sdk/cpp/data.hpp"   // PodData<T>, IData

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

namespace gn::msg {

// ─── Core capability metadata ─────────────────────────────────────────────────
/// Exchanged inside AuthPayload so both peers know what features the other
/// supports.  This is NOT in types.h because it is payload data, not framing.

/// @brief Feature flags advertised by a running GoodNet core.
#define CORE_CAP_ZSTD   (1U << 0) ///< Payload compression available
#define CORE_CAP_ICE    (1U << 1) ///< ICE/DTLS transport supported
#define CORE_CAP_KEYROT (1U << 2) ///< On-line key rotation supported
#define CORE_CAP_RELAY  (1U << 3) ///< Gossip relay supported

/// Packed core version: (major<<16)|(minor<<8)|patch
#define GN_CORE_VERSION ((1U << 16) | (0U << 8) | 2U)  // 1.0.2

#pragma pack(push, 1)
/// @brief Peer capability block embedded in AuthPayload.
///        Zero-filled by old clients — receiver treats all-zero as "unknown".
struct CoreMeta {
    uint32_t core_version; ///< GN_CORE_VERSION of the sender
    uint32_t caps_mask;    ///< CORE_CAP_* flags
};
#pragma pack(pop)

// ─── AUTH (MSG_TYPE_AUTH = 1) ─────────────────────────────────────────────────
/// Sent immediately after TCP connect, in both directions, in plaintext.
///
/// Wire layout:
///   user_pubkey[32] | device_pubkey[32] | signature[64] | ephem_pubkey[32]
///   | schemes_count[1] | schemes[8][16] | CoreMeta[8]
///
/// Old clients (no schemes / no CoreMeta) send payload_len == kBaseSize.
/// Receivers check payload_len before reading optional sections.

static constexpr uint8_t AUTH_MAX_SCHEMES = 8;
static constexpr uint8_t AUTH_SCHEME_LEN  = 16;

#pragma pack(push, 1)
struct AuthPayload {
    uint8_t user_pubkey  [32];
    uint8_t device_pubkey[32];
    uint8_t signature    [64];  ///< Ed25519(user_sk, user_pk||device_pk||ephem_pk)
    uint8_t ephem_pubkey [32];

    // ── Optional section (old clients stop here) ──────────────────────────────
    uint8_t schemes_count;
    char    schemes[AUTH_MAX_SCHEMES][AUTH_SCHEME_LEN];

    CoreMeta core_meta;

    // ── Size constants ────────────────────────────────────────────────────────
    static constexpr size_t kBaseSize    = 32 + 32 + 64 + 32;
    static constexpr size_t kSchemeBlock = 1 + AUTH_MAX_SCHEMES * AUTH_SCHEME_LEN;
    static constexpr size_t kFullSize    = kBaseSize + kSchemeBlock + sizeof(CoreMeta);

    // ── Helpers ───────────────────────────────────────────────────────────────
    void set_schemes(const std::vector<std::string>& sv) {
        schemes_count = static_cast<uint8_t>(
            std::min(sv.size(), static_cast<size_t>(AUTH_MAX_SCHEMES)));
        for (uint8_t i = 0; i < schemes_count; ++i) {
            std::strncpy(schemes[i], sv[i].c_str(), AUTH_SCHEME_LEN - 1);
            schemes[i][AUTH_SCHEME_LEN - 1] = '\0';
        }
        for (uint8_t i = schemes_count; i < AUTH_MAX_SCHEMES; ++i)
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

static_assert(sizeof(AuthPayload) == AuthPayload::kFullSize,
              "AuthPayload size mismatch — check CoreMeta or padding");

// ─── HEARTBEAT (MSG_TYPE_HEARTBEAT = 3) ───────────────────────────────────────
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

// ─── KEY_EXCHANGE (MSG_TYPE_KEY_EXCHANGE = 2) ─────────────────────────────────
/// Reserved for future explicit key exchange; current flow uses the ephemeral
/// key embedded in AuthPayload.

#pragma pack(push, 1)
struct KeyExchangePayload {
    uint8_t x25519_pubkey[32]; ///< Ephemeral X25519 public key
    uint8_t signature    [64]; ///< Ed25519(device_sk, x25519_pubkey) — anti-replay
};
#pragma pack(pop)
static_assert(sizeof(KeyExchangePayload) == 96, "KeyExchangePayload size mismatch");

using KeyExchangeMessage = gn::sdk::PodData<KeyExchangePayload>;

// ─── RELAY (MSG_TYPE_RELAY = 10) ───────────────────────────────────────────────
/// Gossip relay wrapper. Encrypted payload after decrypt:
///   ttl(1) | dest_pubkey(32) | inner_frame (header_t + encrypted payload)
/// inner_frame is opaque to relay nodes — forwarded as-is.

#pragma pack(push, 1)
struct RelayPayload {
    uint8_t  ttl;              ///< Remaining hops (decremented, drop at 0)
    uint8_t  dest_pubkey[32];  ///< Final destination user_pubkey
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
    uint8_t  sender_pubkey[32]; ///< Sender's user pubkey
    uint16_t listen_port;       ///< Sender's listening port
    uint8_t  _pad[2];
};
#pragma pack(pop)
static_assert(sizeof(DhtPingPayload) == 44, "DhtPingPayload size mismatch");

#pragma pack(push, 1)
struct DhtFindNodePayload {
    uint64_t request_id;
    uint8_t  target_pubkey[32]; ///< Key we're looking for
    uint8_t  k;                 ///< Number of closest nodes requested
    uint8_t  response_count;    ///< 0 = request, >0 = response with entries
    uint8_t  _pad[2];
    // Followed by response_count * DhtNodeEntry (variable)
};
#pragma pack(pop)

#pragma pack(push, 1)
struct DhtNodeEntry {
    uint8_t  pubkey[32];
    char     address[64];  ///< ip:port
    uint16_t port;
    uint8_t  _pad[2];
};
#pragma pack(pop)

#pragma pack(push, 1)
struct DhtAnnouncePayload {
    uint8_t  pubkey[32];
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
    uint8_t  dest_pubkey[32];  ///< Reachable destination
    uint8_t  via_pubkey[32];   ///< Next hop (announcer)
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
    uint8_t  target_pubkey[32];
    uint8_t  is_response;      ///< 0=query, 1=response
    uint8_t  hop_count;        ///< Hops from responder to target
    uint8_t  _pad[2];
    // If is_response: followed by via_pubkey[32]
};
#pragma pack(pop)

// ─── TUN/TAP ──────────────────────────────────────────────────────────────────

#pragma pack(push, 1)
struct TunConfigPayload {
    uint32_t tunnel_id;
    uint8_t  virtual_ip[4];     ///< Assigned virtual IPv4
    uint8_t  netmask[4];        ///< /24 default
    uint8_t  gateway_pubkey[32]; ///< Mesh gateway
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

} // namespace gn::msg
