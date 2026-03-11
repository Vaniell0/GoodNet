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

#include "types.h"
#include "cpp/data.hpp"   // PodData<T>, IData

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

} // namespace gn::msg
