/// @file plugins/connectors/ice/session.cpp
/// @brief ICE session implementation: gather, check, nominate, relay.

#include "session.hpp"

#include <openssl/rand.h>

#include <algorithm>
#include <cstring>
#include <random>

namespace gn::ice {

// ── Credentials generation ──────────────────────────────────────────────────

static std::string random_alphanumeric(size_t len) {
    static constexpr char chars[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::string result(len, '\0');
    std::array<uint8_t, 64> buf{};
    RAND_bytes(buf.data(), static_cast<int>(std::min(len, buf.size())));
    for (size_t i = 0; i < len; ++i)
        result[i] = chars[buf[i] % (sizeof(chars) - 1)];
    return result;
}

IceCredentials generate_credentials() {
    return {random_alphanumeric(16), random_alphanumeric(24)};
}

uint64_t pair_priority(uint32_t controlling_prio, uint32_t controlled_prio,
                       bool we_are_controlling) {
    uint64_t g = controlling_prio;
    uint64_t d = controlled_prio;
    if (!we_are_controlling) std::swap(g, d);
    uint64_t mn = std::min(g, d);
    uint64_t mx = std::max(g, d);
    return (mn << 32) + 2 * mx + (g > d ? 1 : 0);
}

// ── IceSession ──────────────────────────────────────────────────────────────

IceSession::IceSession(boost::asio::io_context& io,
                       const std::string& peer_hex,
                       bool controlling,
                       const std::string& stun_server, uint16_t stun_port,
                       Callbacks cbs)
    : io_(io)
    , peer_hex_(peer_hex)
    , controlling_(controlling)
    , cbs_(std::move(cbs))
    , local_creds_(generate_credentials())
    , socket_(io, boost::asio::ip::udp::endpoint(
          boost::asio::ip::udp::v4(), 0))
    , stun_client_(io)
    , stun_server_(stun_server)
    , stun_port_(stun_port)
    , check_timer_(io)
    , timeout_timer_(io)
    , keepalive_timer_(io)
{
    // Случайный tie-breaker
    std::array<uint8_t, 8> tb{};
    RAND_bytes(tb.data(), 8);
    std::memcpy(&tie_breaker_, tb.data(), 8);
}

IceSession::~IceSession() {
    close();
}

// ── Gather ──────────────────────────────────────────────────────────────────

void IceSession::gather() {
    if (state_ != SessionState::New) return;
    transition(SessionState::Gathering);

    gather_host_candidates();
    gather_srflx();
}

void IceSession::gather_host_candidates() {
    // Собираем адрес нашего сокета (host candidate)
    auto local_ep = socket_.local_endpoint();

    // Пробуем получить реальные адреса интерфейсов через connect trick
    // Для host candidate используем 0.0.0.0 если нет лучше
    boost::system::error_code ec;

    // Пробуем определить наш IP через connect к STUN серверу (без отправки)
    boost::asio::ip::udp::socket probe(io_, boost::asio::ip::udp::v4());
    boost::asio::ip::udp::resolver resolver(io_);
    auto results = resolver.resolve(stun_server_, std::to_string(stun_port_), ec);

    boost::asio::ip::address local_addr;
    if (!ec && !results.empty()) {
        probe.connect(results.begin()->endpoint(), ec);
        if (!ec) {
            local_addr = probe.local_endpoint().address();
        }
        probe.close(ec);
    }

    if (local_addr.is_unspecified()) {
        // Fallback: используем loopback (для тестов)
        local_addr = boost::asio::ip::make_address("127.0.0.1");
    }

    Candidate host{};
    host.type = CandidateType::Host;
    host.endpoint = boost::asio::ip::udp::endpoint(local_addr, local_ep.port());
    host.priority = Candidate::compute_priority(CandidateType::Host, 65535);
    local_candidates_.push_back(host);
}

void IceSession::gather_srflx() {
    auto self = shared_from_this();
    stun_client_.resolve(stun_server_, stun_port_, socket_,
        [self](std::optional<StunAddress> addr) {
            self->on_srflx_result(std::move(addr));
        });

    // Для того чтобы StunClient::feed работал, запускаем recv loop
    start_recv_loop();
}

void IceSession::on_srflx_result(std::optional<StunAddress> addr) {
    if (addr) {
        Candidate srflx{};
        srflx.type = CandidateType::Srflx;
        srflx.endpoint = boost::asio::ip::udp::endpoint(addr->ip, addr->port);
        srflx.priority = Candidate::compute_priority(CandidateType::Srflx, 65534);
        local_candidates_.push_back(srflx);
    }

    gathering_complete();
}

void IceSession::gathering_complete() {
    if (state_ != SessionState::Gathering) return;

    if (remote_candidates_.empty()) {
        transition(SessionState::WaitingRemote);
    } else {
        // Remote уже известен (answerer получил offer до завершения gather)
        transition(SessionState::Checking);
        form_check_list();
        start_session_timeout();
        send_next_check();
    }

    if (cbs_.on_gathered)
        cbs_.on_gathered(shared_from_this());
}

// ── Remote candidates ───────────────────────────────────────────────────────

void IceSession::set_remote(const IceSignalData& remote,
                             std::span<const IceCandidateWire> candidates) {
    // Сохраняем remote credentials
    remote_creds_.ufrag.assign(remote.ufrag,
        strnlen(remote.ufrag, sizeof(remote.ufrag)));
    remote_creds_.pwd.assign(remote.pwd,
        strnlen(remote.pwd, sizeof(remote.pwd)));

    // Десериализуем кандидаты
    remote_candidates_.clear();
    for (const auto& w : candidates)
        remote_candidates_.push_back(wire_to_candidate(w));

    // Если gathering завершён — начинаем checks
    if (state_ == SessionState::WaitingRemote) {
        transition(SessionState::Checking);
        form_check_list();
        start_session_timeout();
        send_next_check();
    }
}

void IceSession::start_checks() {
    // Может вызываться явно если set_remote пришёл до gathering_complete
    if (state_ == SessionState::WaitingRemote &&
        !remote_candidates_.empty()) {
        transition(SessionState::Checking);
        form_check_list();
        start_session_timeout();
        send_next_check();
    }
}

// ── Check list ──────────────────────────────────────────────────────────────

void IceSession::form_check_list() {
    check_list_.clear();
    next_check_idx_ = 0;

    for (const auto& local : local_candidates_) {
        for (const auto& remote : remote_candidates_) {
            // Пропускаем пары с разными семействами адресов
            if (local.endpoint.address().is_v4() != remote.endpoint.address().is_v4())
                continue;

            CandidatePair pair{};
            pair.local = local;
            pair.remote = remote;
            pair.priority = pair_priority(local.priority, remote.priority, controlling_);
            check_list_.push_back(pair);
        }
    }

    // Сортируем по priority (descending)
    std::sort(check_list_.begin(), check_list_.end(),
              [](const auto& a, const auto& b) { return a.priority > b.priority; });
}

void IceSession::send_next_check() {
    if (state_ != SessionState::Checking || closed_) return;

    // Ищем следующую пару в состоянии Waiting
    while (next_check_idx_ < check_list_.size()) {
        auto& pair = check_list_[next_check_idx_];
        if (pair.state == CandidatePair::Waiting) {
            check_pair(pair);
            ++next_check_idx_;
            break;
        }
        ++next_check_idx_;
    }

    // Проверяем, есть ли ещё пары для проверки
    bool any_waiting = false;
    bool any_in_progress = false;
    for (const auto& p : check_list_) {
        if (p.state == CandidatePair::Waiting) any_waiting = true;
        if (p.state == CandidatePair::InProgress) any_in_progress = true;
    }

    if (!any_waiting && !any_in_progress && !nominated_) {
        // Все пары проверены, ни одна не успешна
        transition(SessionState::Failed);
        if (cbs_.on_failed) cbs_.on_failed(shared_from_this());
        return;
    }

    // Запланировать следующую проверку
    if (any_waiting) {
        check_timer_.expires_after(CHECK_INTERVAL);
        auto self = shared_from_this();
        check_timer_.async_wait([self](const boost::system::error_code& ec) {
            if (!ec) self->send_next_check();
        });
    }
}

void IceSession::check_pair(CandidatePair& pair) {
    pair.state = CandidatePair::InProgress;
    pair.check_txn = random_txn_id();

    std::string username = remote_creds_.ufrag + ":" + local_creds_.ufrag;

    // Controlling отправляет USE-CANDIDATE при aggressive nomination
    bool use_cand = controlling_;

    auto pkt = build_check_request(
        pair.check_txn, username, remote_creds_.pwd,
        pair.local.priority, use_cand, controlling_, tie_breaker_);

    boost::system::error_code ec;
    socket_.send_to(boost::asio::buffer(pkt), pair.remote.endpoint, 0, ec);
}

// ── Recv loop ───────────────────────────────────────────────────────────────

void IceSession::start_recv_loop() {
    if (recv_active_ || closed_) return;
    recv_active_ = true;
    do_recv();
}

void IceSession::do_recv() {
    if (closed_) return;
    auto self = shared_from_this();
    socket_.async_receive_from(
        boost::asio::buffer(recv_buf_), recv_from_,
        [self](const boost::system::error_code& ec, size_t bytes) {
            if (ec) {
                if (ec != boost::asio::error::operation_aborted)
                    self->recv_active_ = false;
                return;
            }
            self->on_recv(bytes, self->recv_from_);
            self->do_recv();
        });
}

void IceSession::on_recv(size_t bytes_read,
                          const boost::asio::ip::udp::endpoint& from) {
    std::span<const uint8_t> data(recv_buf_.data(), bytes_read);

    // Сначала проверяем, нужно ли это StunClient (для STUN server response)
    if (stun_client_.feed(data))
        return;

    if (is_stun_packet(data)) {
        handle_stun_packet(from, data);
    } else if (nominated_ && state_ == SessionState::Connected) {
        // Application data
        if (cbs_.on_data)
            cbs_.on_data(shared_from_this(), data);
    }
}

void IceSession::handle_stun_packet(const boost::asio::ip::udp::endpoint& from,
                                     std::span<const uint8_t> data) {
    if (data.size() < STUN_HEADER_SIZE) return;

    uint16_t msg_type = static_cast<uint16_t>((data[0] << 8) | data[1]);

    if (msg_type == STUN_BINDING_RESPONSE) {
        handle_stun_response(from, data);
    } else if (msg_type == STUN_BINDING_REQUEST) {
        handle_stun_request(from, data);
    }
}

void IceSession::handle_stun_response(const boost::asio::ip::udp::endpoint& from,
                                       std::span<const uint8_t> data) {
    auto result = parse_binding_response(data);
    if (!result) return;

    // Keepalive response — consent freshness
    if (nominated_ && result->txn_id == keepalive_txn_) {
        on_keepalive_response();
        return;
    }

    // Ищем пару по transaction ID
    for (size_t i = 0; i < check_list_.size(); ++i) {
        auto& pair = check_list_[i];
        if (pair.state != CandidatePair::InProgress) continue;
        if (pair.check_txn != result->txn_id) continue;

        pair.state = CandidatePair::Succeeded;

        // Если мы controlling и это первый success — номинируем
        if (controlling_ && !nominated_) {
            nominate(i);
        }
        return;
    }
}

void IceSession::handle_stun_request(const boost::asio::ip::udp::endpoint& from,
                                      std::span<const uint8_t> data) {
    auto req = parse_binding_request(data, local_creds_.pwd);
    if (!req) return;

    // Отправляем response
    StunAddress mapped{from.address(), from.port()};
    auto resp = build_check_response(req->txn_id, mapped, local_creds_.pwd);
    boost::system::error_code ec;
    socket_.send_to(boost::asio::buffer(resp), from, 0, ec);

    // Если получили USE-CANDIDATE и мы controlled — номинируем эту пару
    if (req->use_candidate && !controlling_ && !nominated_) {
        // Ищем пару по remote endpoint
        for (size_t i = 0; i < check_list_.size(); ++i) {
            if (check_list_[i].remote.endpoint == from) {
                check_list_[i].state = CandidatePair::Succeeded;
                nominate(i);
                return;
            }
        }

        // Peer-reflexive case: кандидат не в списке, добавляем
        CandidatePair pair{};
        pair.local = local_candidates_.empty() ? Candidate{} : local_candidates_[0];
        Candidate rc{};
        rc.type = CandidateType::Host;
        rc.endpoint = from;
        rc.priority = req->priority;
        pair.remote = rc;
        pair.state = CandidatePair::Succeeded;
        pair.priority = pair_priority(pair.local.priority, rc.priority, controlling_);
        check_list_.push_back(pair);
        nominate(check_list_.size() - 1);
    }
}

// ── Nomination ──────────────────────────────────────────────────────────────

void IceSession::nominate(size_t pair_idx) {
    if (nominated_ || pair_idx >= check_list_.size()) return;

    nominated_ = true;
    remote_ep_ = check_list_[pair_idx].remote.endpoint;

    check_timer_.cancel();
    timeout_timer_.cancel();

    transition(SessionState::Connected);
    start_keepalive();

    if (cbs_.on_connected)
        cbs_.on_connected(shared_from_this());
}

// ── Data ────────────────────────────────────────────────────────────────────

void IceSession::send(std::span<const uint8_t> data) {
    if (!nominated_ || state_ != SessionState::Connected || closed_) return;

    boost::system::error_code ec;
    socket_.send_to(boost::asio::buffer(data.data(), data.size()),
                    remote_ep_, 0, ec);
}

void IceSession::close() {
    if (closed_) return;
    closed_ = true;

    stun_client_.cancel();
    check_timer_.cancel();
    timeout_timer_.cancel();
    keepalive_timer_.cancel();

    boost::system::error_code ec;
    socket_.close(ec);
}

// ── Serialize signal ────────────────────────────────────────────────────────

std::vector<uint8_t> IceSession::serialize_signal() const {
    size_t cand_count = local_candidates_.size();
    size_t total = sizeof(IceSignalData) + cand_count * sizeof(IceCandidateWire);

    std::vector<uint8_t> buf(total, 0);

    auto* hdr = reinterpret_cast<IceSignalData*>(buf.data());
    std::memset(hdr->ufrag, 0, sizeof(hdr->ufrag));
    std::memset(hdr->pwd, 0, sizeof(hdr->pwd));
    std::memcpy(hdr->ufrag, local_creds_.ufrag.data(),
                std::min(local_creds_.ufrag.size(), sizeof(hdr->ufrag)));
    std::memcpy(hdr->pwd, local_creds_.pwd.data(),
                std::min(local_creds_.pwd.size(), sizeof(hdr->pwd)));
    hdr->candidate_count = static_cast<uint16_t>(cand_count);
    hdr->_pad = 0;

    auto* wire = reinterpret_cast<IceCandidateWire*>(
        buf.data() + sizeof(IceSignalData));
    for (size_t i = 0; i < cand_count; ++i)
        wire[i] = candidate_to_wire(local_candidates_[i]);

    return buf;
}

// ── State transitions ───────────────────────────────────────────────────────

void IceSession::transition(SessionState to) {
    state_ = to;
}

// ── Keepalive + consent freshness (RFC 7675) ────────────────────────────────

void IceSession::start_keepalive() {
    consent_missed_ = 0;
    send_keepalive();
}

void IceSession::send_keepalive() {
    if (!nominated_ || closed_ || state_ != SessionState::Connected) return;

    // Новый transaction ID для каждого keepalive
    keepalive_txn_ = random_txn_id();

    std::string username = remote_creds_.ufrag + ":" + local_creds_.ufrag;
    auto pkt = build_check_request(
        keepalive_txn_, username, remote_creds_.pwd,
        local_candidates_.empty() ? 0 : local_candidates_[0].priority,
        false /* не USE-CANDIDATE */, controlling_, tie_breaker_);

    boost::system::error_code ec;
    socket_.send_to(boost::asio::buffer(pkt), remote_ep_, 0, ec);

    ++consent_missed_;

    // Планируем следующий keepalive
    keepalive_timer_.expires_after(KEEPALIVE_INTERVAL);
    auto self = shared_from_this();
    keepalive_timer_.async_wait([self](const boost::system::error_code& ec) {
        if (ec) return;
        if (self->consent_missed_ >= CONSENT_MAX_FAILURES) {
            // Consent expired — peer не отвечает
            self->keepalive_timer_.cancel();
            self->transition(SessionState::Failed);
            if (self->cbs_.on_failed)
                self->cbs_.on_failed(self->shared_from_this());
            return;
        }
        self->send_keepalive();
    });
}

void IceSession::on_keepalive_response() {
    consent_missed_ = 0;
}

// ── Session timeout ─────────────────────────────────────────────────────────

void IceSession::start_session_timeout() {
    timeout_timer_.expires_after(SESSION_TIMEOUT);
    auto self = shared_from_this();
    timeout_timer_.async_wait([self](const boost::system::error_code& ec) {
        if (ec) return;
        if (self->state_ == SessionState::Checking && !self->nominated_) {
            self->transition(SessionState::Failed);
            if (self->cbs_.on_failed)
                self->cbs_.on_failed(self->shared_from_this());
        }
    });
}

} // namespace gn::ice
