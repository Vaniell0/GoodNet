#pragma once
/// @file plugins/connectors/ice/candidate.hpp
/// @brief ICE candidate types, credentials, pair logic, binary signal format.

#include "stun.hpp"

#include <boost/asio.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace gn::ice {

// ── Candidate types ─────────────────────────────────────────────────────────

enum class CandidateType : uint8_t {
    Host  = 0,
    Srflx = 1,  // Server reflexive (через STUN)
};

struct Candidate {
    CandidateType type;
    boost::asio::ip::udp::endpoint endpoint;
    uint32_t priority = 0;
    uint32_t component_id = 1;

    /// RFC 8445 §5.1.2.1:
    /// priority = (2^24) * type_pref + (2^8) * local_pref + (256 - component_id)
    static uint32_t compute_priority(CandidateType type, uint16_t local_pref) {
        uint32_t type_pref = (type == CandidateType::Host) ? 126 : 100;
        return (type_pref << 24) | (static_cast<uint32_t>(local_pref) << 8) | 255;
    }
};

// ── ICE credentials ─────────────────────────────────────────────────────────

struct IceCredentials {
    std::string ufrag;  ///< 16 random alphanumeric chars
    std::string pwd;    ///< 24 random alphanumeric chars
};

/// Генерировать случайные ICE credentials.
IceCredentials generate_credentials();

// ── Candidate pair ──────────────────────────────────────────────────────────

struct CandidatePair {
    Candidate local;
    Candidate remote;
    uint64_t priority = 0;
    enum State : uint8_t { Waiting, InProgress, Succeeded, Failed } state = Waiting;
    TransactionId check_txn{};
    int retries = 0;
};

/// RFC 8445 §6.1.2.3:
/// pair_priority = 2^32 * MIN(G,D) + 2 * MAX(G,D) + (G > D ? 1 : 0)
/// G = controlling priority, D = controlled priority
uint64_t pair_priority(uint32_t controlling_prio, uint32_t controlled_prio,
                       bool we_are_controlling);

// ── Binary signal wire format ───────────────────────────────────────────────

#pragma pack(push, 1)

/// Бинарный формат ICE signal данных (variable part после IceSignalPayload).
/// Заменяет SDP текст (124 байт вместо ~1000).
struct IceSignalData {
    char     ufrag[16];
    char     pwd[24];
    uint16_t candidate_count;
    uint16_t _pad;
    // Followed by: candidate_count * IceCandidateWire
};

/// Один кандидат в wire формате.
struct IceCandidateWire {
    uint8_t  type;       ///< CandidateType
    uint8_t  family;     ///< 4 = IPv4, 6 = IPv6
    uint16_t port;
    uint32_t priority;
    uint8_t  addr[16];   ///< IPv4: первые 4 байта, IPv6: все 16
};

#pragma pack(pop)

static_assert(sizeof(IceSignalData) == 44);
static_assert(sizeof(IceCandidateWire) == 24);

// ── Serialization helpers ───────────────────────────────────────────────────

/// Сериализовать кандидат в wire формат.
inline IceCandidateWire candidate_to_wire(const Candidate& c) {
    IceCandidateWire w{};
    w.type = static_cast<uint8_t>(c.type);
    w.priority = c.priority;

    if (c.endpoint.address().is_v4()) {
        w.family = 4;
        w.port = c.endpoint.port();
        auto bytes = c.endpoint.address().to_v4().to_bytes();
        std::memcpy(w.addr, bytes.data(), 4);
    } else {
        w.family = 6;
        w.port = c.endpoint.port();
        auto bytes = c.endpoint.address().to_v6().to_bytes();
        std::memcpy(w.addr, bytes.data(), 16);
    }
    return w;
}

/// Десериализовать wire формат в Candidate.
inline Candidate wire_to_candidate(const IceCandidateWire& w) {
    Candidate c{};
    c.type = static_cast<CandidateType>(w.type);
    c.priority = w.priority;

    if (w.family == 4) {
        auto addr = boost::asio::ip::make_address_v4(
            std::array<uint8_t, 4>{w.addr[0], w.addr[1], w.addr[2], w.addr[3]});
        c.endpoint = boost::asio::ip::udp::endpoint(addr, w.port);
    } else {
        std::array<uint8_t, 16> v6{};
        std::memcpy(v6.data(), w.addr, 16);
        auto addr = boost::asio::ip::make_address_v6(v6);
        c.endpoint = boost::asio::ip::udp::endpoint(addr, w.port);
    }
    return c;
}

} // namespace gn::ice
