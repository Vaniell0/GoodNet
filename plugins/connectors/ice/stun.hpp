#pragma once
/// @file plugins/connectors/ice/stun.hpp
/// @brief STUN protocol (RFC 5389) — builder, parser, async client.

#include <boost/asio.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gn::ice {

// ── STUN constants ──────────────────────────────────────────────────────────

static constexpr uint16_t STUN_BINDING_REQUEST  = 0x0001;
static constexpr uint16_t STUN_BINDING_RESPONSE = 0x0101;
static constexpr uint16_t STUN_BINDING_ERROR    = 0x0111;

static constexpr uint16_t STUN_ATTR_MAPPED_ADDRESS     = 0x0001;
static constexpr uint16_t STUN_ATTR_USERNAME           = 0x0006;
static constexpr uint16_t STUN_ATTR_MESSAGE_INTEGRITY  = 0x0008;
static constexpr uint16_t STUN_ATTR_XOR_MAPPED_ADDRESS = 0x0020;
static constexpr uint16_t STUN_ATTR_PRIORITY           = 0x0024;
static constexpr uint16_t STUN_ATTR_USE_CANDIDATE      = 0x0025;
static constexpr uint16_t STUN_ATTR_FINGERPRINT        = 0x8028;
static constexpr uint16_t STUN_ATTR_ICE_CONTROLLED     = 0x8029;
static constexpr uint16_t STUN_ATTR_ICE_CONTROLLING    = 0x802A;

static constexpr uint32_t STUN_MAGIC_COOKIE = 0x2112A442;
static constexpr size_t   STUN_HEADER_SIZE  = 20;
static constexpr size_t   STUN_HMAC_SIZE    = 20;  // SHA-1

using TransactionId = std::array<uint8_t, 12>;

// ── Types ───────────────────────────────────────────────────────────────────

struct StunAddress {
    boost::asio::ip::address ip;
    uint16_t port = 0;
};

struct StunResult {
    StunAddress   mapped;
    TransactionId txn_id;
};

/// Входящий STUN Binding Request (для connectivity checks).
struct StunRequest {
    TransactionId txn_id;
    uint32_t      priority = 0;
    bool          use_candidate = false;
    std::string   username;
};

// ── Builder functions ───────────────────────────────────────────────────────

/// Построить простой STUN Binding Request (для STUN server resolve).
std::vector<uint8_t> build_binding_request(TransactionId& txn_id_out);

/// Построить STUN Binding Request с integrity (ICE connectivity check).
std::vector<uint8_t> build_check_request(
    const TransactionId& txn_id,
    const std::string& username,      // "remote_ufrag:local_ufrag"
    const std::string& password,      // remote password (for HMAC)
    uint32_t priority,
    bool use_candidate,
    bool controlling,
    uint64_t tie_breaker);

/// Построить STUN Binding Response (ответ на connectivity check).
std::vector<uint8_t> build_check_response(
    const TransactionId& txn_id,
    const StunAddress& mapped_addr,
    const std::string& password);

// ── Parser functions ────────────────────────────────────────────────────────

/// Разобрать STUN Binding Response. nullopt при ошибке.
std::optional<StunResult> parse_binding_response(std::span<const uint8_t> data);

/// Разобрать входящий STUN Binding Request. Проверяет MESSAGE-INTEGRITY.
std::optional<StunRequest> parse_binding_request(
    std::span<const uint8_t> data,
    const std::string& expected_password);

/// Проверить, является ли пакет STUN (по magic cookie).
bool is_stun_packet(std::span<const uint8_t> data);

// ── Async STUN Client ───────────────────────────────────────────────────────

/// Async STUN client: resolve mapped address через STUN Binding Request.
class StunClient {
public:
    using Callback = std::function<void(std::optional<StunAddress>)>;

    explicit StunClient(boost::asio::io_context& io);

    /// Отправить STUN Binding Request через существующий сокет.
    /// Вызывает cb с результатом (или nullopt при таймауте).
    void resolve(const std::string& stun_server, uint16_t stun_port,
                 boost::asio::ip::udp::socket& sock, Callback cb);

    /// Обработать входящий пакет (может быть STUN response).
    /// Возвращает true если пакет был обработан.
    bool feed(std::span<const uint8_t> data);

    void cancel();

private:
    boost::asio::io_context& io_;
    boost::asio::steady_timer timer_;
    TransactionId pending_txn_{};
    Callback pending_cb_;
    boost::asio::ip::udp::socket* pending_sock_ = nullptr;
    boost::asio::ip::udp::endpoint stun_ep_;
    int retries_ = 0;
    bool active_ = false;

    static constexpr int MAX_RETRIES = 3;
    static constexpr auto RETRY_INTERVAL = std::chrono::milliseconds(500);

    void send_request();
    void schedule_retry();
    void finish(std::optional<StunAddress> result);
};

// ── Helpers ─────────────────────────────────────────────────────────────────

/// Генерировать случайный Transaction ID.
TransactionId random_txn_id();

/// CRC32 для STUN FINGERPRINT (XOR 0x5354554E).
uint32_t stun_fingerprint(std::span<const uint8_t> data);

/// HMAC-SHA1 для STUN MESSAGE-INTEGRITY.
std::array<uint8_t, 20> stun_hmac_sha1(
    std::span<const uint8_t> data,
    const std::string& key);

} // namespace gn::ice
