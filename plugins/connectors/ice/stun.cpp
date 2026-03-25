/// @file plugins/connectors/ice/stun.cpp
/// @brief STUN protocol implementation (RFC 5389).

#include "stun.hpp"

#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <algorithm>
#include <cstring>
#include <numeric>

namespace gn::ice {

// ── Endian helpers ──────────────────────────────────────────────────────────

static uint16_t read_u16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

static uint32_t read_u32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8)  |
            static_cast<uint32_t>(p[3]);
}

static void write_u16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}

static void write_u32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

static void write_u64(uint8_t* p, uint64_t v) {
    for (int i = 7; i >= 0; --i) {
        p[i] = static_cast<uint8_t>(v);
        v >>= 8;
    }
}

// ── CRC32 (STUN FINGERPRINT) ───────────────────────────────────────────────

// RFC 5389 §15.5: CRC32 по полиному 0xEDB88320 (ISO 3309 / ITU-T V.42)
static uint32_t crc32_table[256];
static bool crc32_init = false;

static void init_crc32_table() {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t crc = i;
        for (int j = 0; j < 8; ++j)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320u : 0);
        crc32_table[i] = crc;
    }
    crc32_init = true;
}

static uint32_t compute_crc32(const uint8_t* data, size_t len) {
    if (!crc32_init) init_crc32_table();
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; ++i)
        crc = (crc >> 8) ^ crc32_table[(crc ^ data[i]) & 0xFF];
    return crc ^ 0xFFFFFFFF;
}

uint32_t stun_fingerprint(std::span<const uint8_t> data) {
    return compute_crc32(data.data(), data.size()) ^ 0x5354554Eu;
}

// ── HMAC-SHA1 ───────────────────────────────────────────────────────────────

std::array<uint8_t, 20> stun_hmac_sha1(
        std::span<const uint8_t> data, const std::string& key) {
    std::array<uint8_t, 20> out{};
    unsigned int len = 20;
    HMAC(EVP_sha1(),
         key.data(), static_cast<int>(key.size()),
         data.data(), data.size(),
         out.data(), &len);
    return out;
}

// ── Random Transaction ID ───────────────────────────────────────────────────

TransactionId random_txn_id() {
    TransactionId id{};
    RAND_bytes(id.data(), static_cast<int>(id.size()));
    return id;
}

// ── STUN attribute helpers ──────────────────────────────────────────────────

// Добавить атрибут в буфер. value_len паддится до 4-byte boundary.
static void append_attr(std::vector<uint8_t>& buf, uint16_t type,
                        const uint8_t* value, uint16_t value_len) {
    size_t pos = buf.size();
    uint16_t padded = (value_len + 3u) & ~3u;
    buf.resize(pos + 4 + padded, 0);
    write_u16(buf.data() + pos, type);
    write_u16(buf.data() + pos + 2, value_len);
    if (value_len > 0)
        std::memcpy(buf.data() + pos + 4, value, value_len);
}

static void append_attr_u32(std::vector<uint8_t>& buf, uint16_t type, uint32_t v) {
    uint8_t tmp[4];
    write_u32(tmp, v);
    append_attr(buf, type, tmp, 4);
}

static void append_attr_u64(std::vector<uint8_t>& buf, uint16_t type, uint64_t v) {
    uint8_t tmp[8];
    write_u64(tmp, v);
    append_attr(buf, type, tmp, 8);
}

// Обновить Message Length в STUN header (buf[2..3]).
static void update_msg_length(std::vector<uint8_t>& buf) {
    uint16_t len = static_cast<uint16_t>(buf.size() - STUN_HEADER_SIZE);
    write_u16(buf.data() + 2, len);
}

// Написать STUN header.
static void write_header(std::vector<uint8_t>& buf, uint16_t type,
                          const TransactionId& txn) {
    buf.resize(STUN_HEADER_SIZE, 0);
    write_u16(buf.data(), type);
    write_u16(buf.data() + 2, 0); // длина обновится потом
    write_u32(buf.data() + 4, STUN_MAGIC_COOKIE);
    std::memcpy(buf.data() + 8, txn.data(), 12);
}

// Добавить MESSAGE-INTEGRITY + FINGERPRINT.
static void append_integrity_and_fingerprint(
        std::vector<uint8_t>& buf, const std::string& password) {
    // MESSAGE-INTEGRITY: HMAC-SHA1 over message up to (but not including) this attr.
    // Нужно выставить Message Length так, чтобы он включал MESSAGE-INTEGRITY.
    uint16_t mi_len = static_cast<uint16_t>(
        buf.size() - STUN_HEADER_SIZE + 4 + STUN_HMAC_SIZE);
    write_u16(buf.data() + 2, mi_len);

    auto hmac = stun_hmac_sha1({buf.data(), buf.size()}, password);
    append_attr(buf, STUN_ATTR_MESSAGE_INTEGRITY, hmac.data(), STUN_HMAC_SIZE);

    // FINGERPRINT: CRC32 over everything before this attr.
    uint16_t fp_len = static_cast<uint16_t>(buf.size() - STUN_HEADER_SIZE + 8);
    write_u16(buf.data() + 2, fp_len);

    uint32_t fp = stun_fingerprint({buf.data(), buf.size()});
    append_attr_u32(buf, STUN_ATTR_FINGERPRINT, fp);

    // Финальная длина
    update_msg_length(buf);
}

// ── Builder: simple Binding Request ─────────────────────────────────────────

std::vector<uint8_t> build_binding_request(TransactionId& txn_id_out) {
    txn_id_out = random_txn_id();
    std::vector<uint8_t> buf;
    write_header(buf, STUN_BINDING_REQUEST, txn_id_out);
    update_msg_length(buf);
    return buf;
}

// ── Builder: ICE connectivity check request ─────────────────────────────────

std::vector<uint8_t> build_check_request(
        const TransactionId& txn_id,
        const std::string& username,
        const std::string& password,
        uint32_t priority,
        bool use_candidate,
        bool controlling,
        uint64_t tie_breaker) {
    std::vector<uint8_t> buf;
    write_header(buf, STUN_BINDING_REQUEST, txn_id);

    // USERNAME
    append_attr(buf, STUN_ATTR_USERNAME,
                reinterpret_cast<const uint8_t*>(username.data()),
                static_cast<uint16_t>(username.size()));

    // PRIORITY
    append_attr_u32(buf, STUN_ATTR_PRIORITY, priority);

    // ICE-CONTROLLING or ICE-CONTROLLED
    if (controlling)
        append_attr_u64(buf, STUN_ATTR_ICE_CONTROLLING, tie_breaker);
    else
        append_attr_u64(buf, STUN_ATTR_ICE_CONTROLLED, tie_breaker);

    // USE-CANDIDATE (zero-length attribute)
    if (use_candidate)
        append_attr(buf, STUN_ATTR_USE_CANDIDATE, nullptr, 0);

    // MESSAGE-INTEGRITY + FINGERPRINT
    append_integrity_and_fingerprint(buf, password);

    return buf;
}

// ── Builder: ICE check response ─────────────────────────────────────────────

std::vector<uint8_t> build_check_response(
        const TransactionId& txn_id,
        const StunAddress& mapped_addr,
        const std::string& password) {
    std::vector<uint8_t> buf;
    write_header(buf, STUN_BINDING_RESPONSE, txn_id);

    // XOR-MAPPED-ADDRESS
    if (mapped_addr.ip.is_v4()) {
        uint8_t xma[8];
        xma[0] = 0; // reserved
        xma[1] = 0x01; // family: IPv4
        uint16_t xport = mapped_addr.port ^ static_cast<uint16_t>(STUN_MAGIC_COOKIE >> 16);
        write_u16(xma + 2, xport);
        auto v4 = mapped_addr.ip.to_v4().to_bytes();
        uint32_t xaddr = read_u32(v4.data()) ^ STUN_MAGIC_COOKIE;
        write_u32(xma + 4, xaddr);
        append_attr(buf, STUN_ATTR_XOR_MAPPED_ADDRESS, xma, 8);
    } else {
        uint8_t xma[20];
        xma[0] = 0;
        xma[1] = 0x02; // family: IPv6
        uint16_t xport = mapped_addr.port ^ static_cast<uint16_t>(STUN_MAGIC_COOKIE >> 16);
        write_u16(xma + 2, xport);
        auto v6 = mapped_addr.ip.to_v6().to_bytes();
        // XOR with magic cookie + transaction id
        uint8_t xor_key[16];
        write_u32(xor_key, STUN_MAGIC_COOKIE);
        std::memcpy(xor_key + 4, txn_id.data(), 12);
        for (int i = 0; i < 16; ++i)
            xma[4 + i] = v6[i] ^ xor_key[i];
        append_attr(buf, STUN_ATTR_XOR_MAPPED_ADDRESS, xma, 20);
    }

    append_integrity_and_fingerprint(buf, password);

    return buf;
}

// ── Parser: Binding Response ────────────────────────────────────────────────

std::optional<StunResult> parse_binding_response(std::span<const uint8_t> data) {
    if (data.size() < STUN_HEADER_SIZE)
        return std::nullopt;

    uint16_t msg_type = read_u16(data.data());
    if (msg_type != STUN_BINDING_RESPONSE)
        return std::nullopt;

    uint32_t cookie = read_u32(data.data() + 4);
    if (cookie != STUN_MAGIC_COOKIE)
        return std::nullopt;

    uint16_t msg_len = read_u16(data.data() + 2);
    if (data.size() < STUN_HEADER_SIZE + msg_len)
        return std::nullopt;

    StunResult result{};
    std::memcpy(result.txn_id.data(), data.data() + 8, 12);

    // Парсим атрибуты
    size_t pos = STUN_HEADER_SIZE;
    size_t end = STUN_HEADER_SIZE + msg_len;
    bool found_mapped = false;

    while (pos + 4 <= end) {
        uint16_t attr_type = read_u16(data.data() + pos);
        uint16_t attr_len  = read_u16(data.data() + pos + 2);
        size_t attr_padded = (attr_len + 3u) & ~3u;

        if (pos + 4 + attr_padded > end)
            break;

        const uint8_t* attr_data = data.data() + pos + 4;

        if (attr_type == STUN_ATTR_XOR_MAPPED_ADDRESS && attr_len >= 8) {
            uint8_t family = attr_data[1];
            uint16_t xport = read_u16(attr_data + 2);
            result.mapped.port = xport ^ static_cast<uint16_t>(STUN_MAGIC_COOKIE >> 16);

            if (family == 0x01 && attr_len >= 8) {
                // IPv4
                uint32_t xaddr = read_u32(attr_data + 4);
                uint32_t addr  = xaddr ^ STUN_MAGIC_COOKIE;
                uint8_t ab[4];
                write_u32(ab, addr);
                result.mapped.ip = boost::asio::ip::make_address_v4(
                    std::array<uint8_t, 4>{ab[0], ab[1], ab[2], ab[3]});
                found_mapped = true;
            } else if (family == 0x02 && attr_len >= 20) {
                // IPv6
                uint8_t xor_key[16];
                write_u32(xor_key, STUN_MAGIC_COOKIE);
                std::memcpy(xor_key + 4, result.txn_id.data(), 12);
                std::array<uint8_t, 16> v6{};
                for (int i = 0; i < 16; ++i)
                    v6[i] = attr_data[4 + i] ^ xor_key[i];
                result.mapped.ip = boost::asio::ip::make_address_v6(v6);
                found_mapped = true;
            }
        } else if (attr_type == STUN_ATTR_MAPPED_ADDRESS && !found_mapped && attr_len >= 8) {
            // Fallback: non-XOR MAPPED-ADDRESS
            uint8_t family = attr_data[1];
            result.mapped.port = read_u16(attr_data + 2);
            if (family == 0x01) {
                result.mapped.ip = boost::asio::ip::make_address_v4(
                    std::array<uint8_t, 4>{
                        attr_data[4], attr_data[5], attr_data[6], attr_data[7]});
                found_mapped = true;
            }
        }

        pos += 4 + attr_padded;
    }

    return found_mapped ? std::optional{result} : std::nullopt;
}

// ── Parser: Binding Request (ICE connectivity check) ────────────────────────

std::optional<StunRequest> parse_binding_request(
        std::span<const uint8_t> data,
        const std::string& expected_password) {
    if (data.size() < STUN_HEADER_SIZE)
        return std::nullopt;

    uint16_t msg_type = read_u16(data.data());
    if (msg_type != STUN_BINDING_REQUEST)
        return std::nullopt;

    uint32_t cookie = read_u32(data.data() + 4);
    if (cookie != STUN_MAGIC_COOKIE)
        return std::nullopt;

    uint16_t msg_len = read_u16(data.data() + 2);
    if (data.size() < STUN_HEADER_SIZE + msg_len)
        return std::nullopt;

    StunRequest req{};
    std::memcpy(req.txn_id.data(), data.data() + 8, 12);

    size_t pos = STUN_HEADER_SIZE;
    size_t end = STUN_HEADER_SIZE + msg_len;
    size_t mi_offset = 0; // позиция MESSAGE-INTEGRITY attr

    while (pos + 4 <= end) {
        uint16_t attr_type = read_u16(data.data() + pos);
        uint16_t attr_len  = read_u16(data.data() + pos + 2);
        size_t attr_padded = (attr_len + 3u) & ~3u;

        if (pos + 4 + attr_padded > end)
            break;

        const uint8_t* attr_data = data.data() + pos + 4;

        switch (attr_type) {
            case STUN_ATTR_USERNAME:
                req.username.assign(reinterpret_cast<const char*>(attr_data), attr_len);
                break;
            case STUN_ATTR_PRIORITY:
                if (attr_len >= 4)
                    req.priority = read_u32(attr_data);
                break;
            case STUN_ATTR_USE_CANDIDATE:
                req.use_candidate = true;
                break;
            case STUN_ATTR_MESSAGE_INTEGRITY:
                mi_offset = pos;
                break;
            default:
                break;
        }

        pos += 4 + attr_padded;
    }

    // Проверяем MESSAGE-INTEGRITY
    if (mi_offset > 0 && !expected_password.empty()) {
        // Пересчитываем HMAC по данным до MESSAGE-INTEGRITY
        // Подменяем Message Length, чтобы включал MI
        std::vector<uint8_t> check(data.begin(), data.begin() + mi_offset);
        uint16_t mi_msg_len = static_cast<uint16_t>(
            mi_offset - STUN_HEADER_SIZE + 4 + STUN_HMAC_SIZE);
        write_u16(check.data() + 2, mi_msg_len);

        auto expected_hmac = stun_hmac_sha1({check.data(), check.size()}, expected_password);
        const uint8_t* actual_hmac = data.data() + mi_offset + 4;

        if (std::memcmp(expected_hmac.data(), actual_hmac, STUN_HMAC_SIZE) != 0)
            return std::nullopt; // integrity fail
    }

    return req;
}

// ── is_stun_packet ──────────────────────────────────────────────────────────

bool is_stun_packet(std::span<const uint8_t> data) {
    // RFC 7983: first two bits must be 00, and magic cookie at offset 4
    if (data.size() < STUN_HEADER_SIZE)
        return false;
    if ((data[0] & 0xC0) != 0x00)
        return false;
    return read_u32(data.data() + 4) == STUN_MAGIC_COOKIE;
}

// ── StunClient ──────────────────────────────────────────────────────────────

StunClient::StunClient(boost::asio::io_context& io)
    : io_(io), timer_(io) {}

void StunClient::resolve(const std::string& stun_server, uint16_t stun_port,
                          boost::asio::ip::udp::socket& sock, Callback cb) {
    if (active_) {
        // Уже есть pending resolve — вызываем предыдущий cb с nullopt
        finish(std::nullopt);
    }

    pending_cb_ = std::move(cb);
    pending_sock_ = &sock;
    retries_ = 0;
    active_ = true;

    // Резолвим STUN server
    boost::asio::ip::udp::resolver resolver(io_);
    boost::system::error_code ec;
    auto results = resolver.resolve(stun_server, std::to_string(stun_port), ec);
    if (ec || results.empty()) {
        finish(std::nullopt);
        return;
    }
    stun_ep_ = *results.begin();

    send_request();
}

bool StunClient::feed(std::span<const uint8_t> data) {
    if (!active_) return false;
    if (!is_stun_packet(data)) return false;

    auto result = parse_binding_response(data);
    if (!result) return false;

    // Проверяем transaction ID
    if (result->txn_id != pending_txn_) return false;

    timer_.cancel();
    finish(result->mapped);
    return true;
}

void StunClient::cancel() {
    if (active_) {
        timer_.cancel();
        finish(std::nullopt);
    }
}

void StunClient::send_request() {
    auto pkt = build_binding_request(pending_txn_);
    boost::system::error_code ec;
    pending_sock_->send_to(boost::asio::buffer(pkt), stun_ep_, 0, ec);
    if (ec) {
        finish(std::nullopt);
        return;
    }
    schedule_retry();
}

void StunClient::schedule_retry() {
    timer_.expires_after(RETRY_INTERVAL);
    timer_.async_wait([this](const boost::system::error_code& ec) {
        if (ec) return; // cancelled
        if (!active_) return;
        if (++retries_ >= MAX_RETRIES) {
            finish(std::nullopt);
            return;
        }
        // Retransmit
        auto pkt = build_binding_request(pending_txn_);
        boost::system::error_code send_ec;
        pending_sock_->send_to(boost::asio::buffer(pkt), stun_ep_, 0, send_ec);
        schedule_retry();
    });
}

void StunClient::finish(std::optional<StunAddress> result) {
    active_ = false;
    pending_sock_ = nullptr;
    if (auto cb = std::exchange(pending_cb_, nullptr))
        cb(result);
}

} // namespace gn::ice
