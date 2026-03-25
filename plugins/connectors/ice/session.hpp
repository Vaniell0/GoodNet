#pragma once
/// @file plugins/connectors/ice/session.hpp
/// @brief ICE session: candidate gathering, connectivity checks, data relay.

#include "stun.hpp"
#include "candidate.hpp"

#include <boost/asio.hpp>
#include <types.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gn::ice {

// ── Session state machine ───────────────────────────────────────────────────

enum class SessionState : uint8_t {
    New,            ///< Создана, ещё не начали gather
    Gathering,      ///< Собираем host + srflx кандидаты
    WaitingRemote,  ///< Ждём remote candidates (signaling)
    Checking,       ///< Connectivity checks в процессе
    Connected,      ///< Номинирована пара, данные идут
    Failed,         ///< Все пары failed или таймаут
};

// ── IceSession ──────────────────────────────────────────────────────────────

class IceSession : public std::enable_shared_from_this<IceSession> {
public:
    /// Callbacks от сессии к IceConnector.
    struct Callbacks {
        std::function<void(std::shared_ptr<IceSession>)>                on_gathered;
        std::function<void(std::shared_ptr<IceSession>)>                on_connected;
        std::function<void(std::shared_ptr<IceSession>)>                on_failed;
        std::function<void(std::shared_ptr<IceSession>,
                           std::span<const uint8_t>)>                   on_data;
    };

    IceSession(boost::asio::io_context& io,
               const std::string& peer_hex,
               bool controlling,
               const std::string& stun_server, uint16_t stun_port,
               Callbacks cbs);
    ~IceSession();

    IceSession(const IceSession&) = delete;
    IceSession& operator=(const IceSession&) = delete;

    // ── Lifecycle ────────────────────────────────────────────────────────────

    /// Начать сбор кандидатов (host + srflx через STUN).
    void gather();

    /// Установить remote credentials + candidates.
    void set_remote(const IceSignalData& remote,
                    std::span<const IceCandidateWire> candidates);

    /// Начать connectivity checks (вызывается после gather + set_remote).
    void start_checks();

    // ── Data ─────────────────────────────────────────────────────────────────

    /// Отправить данные через nominated path.
    void send(std::span<const uint8_t> data);

    /// Закрыть сессию.
    void close();

    // ── Accessors ────────────────────────────────────────────────────────────

    SessionState state() const { return state_; }
    const std::string& peer_hex() const { return peer_hex_; }
    bool controlling() const { return controlling_; }

    /// Сериализовать local credentials + candidates для signaling.
    std::vector<uint8_t> serialize_signal() const;

    /// conn_id назначается IceConnector после connected.
    conn_id_t conn_id = CONN_ID_INVALID;

private:
    boost::asio::io_context& io_;
    std::string peer_hex_;
    bool controlling_;
    SessionState state_ = SessionState::New;
    Callbacks cbs_;

    // ICE credentials
    IceCredentials local_creds_;
    IceCredentials remote_creds_;

    // UDP socket (один на сессию, привязан к 0.0.0.0:0)
    boost::asio::ip::udp::socket socket_;

    // Candidates
    std::vector<Candidate> local_candidates_;
    std::vector<Candidate> remote_candidates_;
    std::vector<CandidatePair> check_list_;

    // Nominated pair
    bool nominated_ = false;
    boost::asio::ip::udp::endpoint remote_ep_;

    // STUN
    StunClient stun_client_;
    std::string stun_server_;
    uint16_t stun_port_;
    uint64_t tie_breaker_;

    // Checking state
    size_t next_check_idx_ = 0;
    boost::asio::steady_timer check_timer_;
    boost::asio::steady_timer timeout_timer_;

    static constexpr auto CHECK_INTERVAL  = std::chrono::milliseconds(50);
    static constexpr auto SESSION_TIMEOUT = std::chrono::seconds(10);
    static constexpr int  CHECK_MAX_RETRIES = 4;

    // Keepalive + consent freshness (RFC 7675)
    boost::asio::steady_timer keepalive_timer_;
    TransactionId keepalive_txn_{};
    int consent_missed_ = 0;

    static constexpr auto KEEPALIVE_INTERVAL    = std::chrono::seconds(20);
    static constexpr int  CONSENT_MAX_FAILURES  = 3;

    // Recv
    std::array<uint8_t, 65536> recv_buf_{};
    boost::asio::ip::udp::endpoint recv_from_;
    bool recv_active_ = false;
    bool closed_ = false;

    // ── Internal ─────────────────────────────────────────────────────────────

    void gather_host_candidates();
    void gather_srflx();
    void on_srflx_result(std::optional<StunAddress> addr);
    void gathering_complete();

    void form_check_list();
    void send_next_check();
    void on_check_timeout();
    void check_pair(CandidatePair& pair);

    void start_recv_loop();
    void do_recv();
    void on_recv(size_t bytes_read, const boost::asio::ip::udp::endpoint& from);
    void handle_stun_packet(const boost::asio::ip::udp::endpoint& from,
                            std::span<const uint8_t> data);
    void handle_stun_response(const boost::asio::ip::udp::endpoint& from,
                              std::span<const uint8_t> data);
    void handle_stun_request(const boost::asio::ip::udp::endpoint& from,
                             std::span<const uint8_t> data);

    void nominate(size_t pair_idx);
    void transition(SessionState to);

    void start_session_timeout();
    void start_keepalive();
    void send_keepalive();
    void on_keepalive_response();
};

} // namespace gn::ice
