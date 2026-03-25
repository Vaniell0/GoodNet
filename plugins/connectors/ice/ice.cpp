/// @file plugins/connectors/ice/ice.cpp
/// @brief Lightweight ICE connector (Boost.Asio, RFC 8445).
///
/// Scheme: "ice"
/// Connect URI: "ice://<peer_pubkey_hex_64>"
///
/// Signaling model:
///   A ──[TCP AUTH]──► B         handled by TCP connector + core
///   A ──[ICE_OFFER]──► B        MSG_TYPE_ICE_SIGNAL, kind=OFFER
///   B ──[ICE_ANSWER]──► A       MSG_TYPE_ICE_SIGNAL, kind=ANSWER
///   A/B ── UDP ──               direct ICE data path
///
/// Threading: Boost.Asio io_context с N потоками (как TCP connector).

#include "session.hpp"
#include "candidate.hpp"

#include <logger.hpp>

#include <cpp/connector.hpp>
#include <connector.h>
#include <handler.h>
#include <messages.hpp>
#include <types.h>

#include <boost/asio.hpp>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace gn {

// ── Wire types ──────────────────────────────────────────────────────────────

using msg::IceSignalKind;
using IceSignalHdr = msg::IceSignalPayload;

namespace asio = boost::asio;

// ── IceConnector ────────────────────────────────────────────────────────────

class IceConnector : public IConnector {
public:
    std::string get_scheme() const override { return "ice"; }
    std::string get_name()   const override { return "GoodNet ICE (Boost.Asio)"; }

    // ── Lifecycle ───────────────────────────────────────────────────────────

    void on_init() override {
        // Parse STUN servers: "host:port,host:port,..." or single "host"
        auto parse_stun_servers = [](const std::string& val) -> std::vector<ice::StunServer> {
            std::vector<ice::StunServer> result;
            size_t pos = 0;
            while (pos < val.size()) {
                size_t comma = val.find(',', pos);
                if (comma == std::string::npos) comma = val.size();
                std::string entry = val.substr(pos, comma - pos);
                // Trim whitespace
                while (!entry.empty() && entry.front() == ' ') entry.erase(0, 1);
                while (!entry.empty() && entry.back() == ' ')  entry.pop_back();
                if (!entry.empty()) {
                    ice::StunServer srv;
                    if (auto colon = entry.rfind(':'); colon != std::string::npos) {
                        srv.host = entry.substr(0, colon);
                        int p = std::atoi(entry.substr(colon + 1).c_str());
                        srv.port = (p > 0 && p <= 65535)
                            ? static_cast<uint16_t>(p) : uint16_t(19302);
                    } else {
                        srv.host = entry;
                    }
                    result.push_back(std::move(srv));
                }
                pos = comma + 1;
            }
            return result;
        };

        // STUN servers: config → env → default
        if (auto v = config_get("ice.stun_servers"); !v.empty()) {
            ice_config_.stun_servers = parse_stun_servers(v);
        } else if (const char* env = std::getenv("GOODNET_STUN_SERVER")) {
            ice::StunServer srv{env, 19302};
            if (const char* env_p = std::getenv("GOODNET_STUN_PORT")) {
                int p = std::atoi(env_p);
                if (p > 0 && p <= 65535) srv.port = static_cast<uint16_t>(p);
            }
            ice_config_.stun_servers = {srv};
        }

        // Configurable timeouts
        if (auto v = config_get("ice.session_timeout"); !v.empty()) {
            int s = std::atoi(v.c_str());
            if (s > 0) ice_config_.session_timeout = std::chrono::seconds(s);
        }
        if (auto v = config_get("ice.keepalive_interval"); !v.empty()) {
            int s = std::atoi(v.c_str());
            if (s > 0) ice_config_.keepalive_interval = std::chrono::seconds(s);
        }
        if (auto v = config_get("ice.consent_failures"); !v.empty()) {
            int n = std::atoi(v.c_str());
            if (n > 0) ice_config_.consent_max_failures = n;
        }

        // io_context + worker threads
        work_.emplace(asio::make_work_guard(io_));
        const int n = std::max(2, static_cast<int>(std::thread::hardware_concurrency()));
        io_threads_.reserve(n);
        for (int i = 0; i < n; ++i)
            io_threads_.emplace_back([this] { io_.run(); });

        // Signal handler для MSG_TYPE_ICE_SIGNAL
        sig_handler_.name                = "ice_signal_handler";
        sig_handler_.user_data           = this;
        sig_handler_.handle_message      = s_on_signal;
        sig_handler_.on_message_result   = nullptr;
        sig_handler_.handle_conn_state   = nullptr;
        sig_handler_.shutdown            = nullptr;
        sig_handler_.supported_types     = &kSigType;
        sig_handler_.num_supported_types = 1;
        register_extra_handler(&sig_handler_);

        LOG_INFO("[ICE] connector ready (STUN servers={}, timeout={}s, {} io threads)",
                 ice_config_.stun_servers.size(),
                 ice_config_.session_timeout.count(), n);
    }

    void on_shutdown() override {
        LOG_INFO("[ICE] shutting down...");

        // Закрыть все сессии
        {
            std::lock_guard lk(mu_);
            for (auto& [k, s] : sessions_)
                s->close();
            sessions_.clear();
        }

        // Остановить io_context
        work_.reset();
        io_.stop();
        for (auto& t : io_threads_)
            if (t.joinable()) t.join();
        io_threads_.clear();
    }

    // ── do_* ────────────────────────────────────────────────────────────────

    int do_connect(const char* uri) override {
        std::string target(uri);
        if (auto p = target.find("://"); p != std::string::npos)
            target = target.substr(p + 3);
        if (target.size() != 64) {
            LOG_ERROR("[ICE] do_connect: need 64-char pubkey hex, got '{}'", target);
            return -1;
        }

        asio::post(io_, [this, peer = std::move(target)] {
            create_session(peer, true /* controlling */);
        });
        return 0;
    }

    int do_listen(const char*, uint16_t) override { return 0; }

    int do_send(conn_id_t id, std::span<const uint8_t> data) override {
        auto buf = std::make_shared<std::vector<uint8_t>>(data.begin(), data.end());
        asio::post(io_, [this, id, buf] {
            auto s = by_conn(id);
            if (!s || s->state() != ice::SessionState::Connected) return;
            s->send({buf->data(), buf->size()});
        });
        return 0;
    }

    void do_close(conn_id_t id, bool /*hard*/) override {
        asio::post(io_, [this, id] {
            std::lock_guard lk(mu_);
            for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
                if (it->second->conn_id != id) continue;
                it->second->close();
                notify_disconnect(id, 0);
                sessions_.erase(it);
                return;
            }
        });
    }

    // ── Incoming ICE signal (from core bus) ─────────────────────────────────

    void on_ice_signal(const endpoint_t* ep, std::span<const uint8_t> pl) {
        if (pl.size() < sizeof(IceSignalHdr)) return;
        const auto* h = reinterpret_cast<const IceSignalHdr*>(pl.data());
        if (pl.size() < sizeof(IceSignalHdr) + h->sdp_len) return;

        const auto kind = static_cast<IceSignalKind>(h->kind);
        const uint8_t* signal_data = pl.data() + sizeof(IceSignalHdr);
        const size_t signal_len = h->sdp_len;

        // Проверяем минимальный размер для бинарного формата
        if (signal_len < sizeof(ice::IceSignalData)) return;

        std::string peer = hex32(ep->pubkey);
        const conn_id_t sig_conn = ep->peer_id;

        // Копируем signal data (будет обработано в io_context)
        auto signal_buf = std::make_shared<std::vector<uint8_t>>(
            signal_data, signal_data + signal_len);

        asio::post(io_, [this, peer, kind, sig_conn, signal_buf] {
            const auto* sd = reinterpret_cast<const ice::IceSignalData*>(
                signal_buf->data());
            size_t expected = sizeof(ice::IceSignalData) +
                              sd->candidate_count * sizeof(ice::IceCandidateWire);
            if (signal_buf->size() < expected) return;

            std::span<const ice::IceCandidateWire> candidates(
                reinterpret_cast<const ice::IceCandidateWire*>(
                    signal_buf->data() + sizeof(ice::IceSignalData)),
                sd->candidate_count);

            if (kind == IceSignalKind::OFFER) {
                // Создаём ответную сессию (controlled)
                auto s = create_session(peer, false /* controlled */);
                if (!s) return;
                s->set_remote(*sd, candidates);
                // gather() уже запущен в create_session
                // После gather → on_gathered отправит ANSWER
            } else {
                // ANSWER: находим сессию, устанавливаем remote
                auto s = by_peer(peer);
                if (!s) return;
                s->set_remote(*sd, candidates);
            }
        });
    }

private:
    // ── Session management ──────────────────────────────────────────────────

    std::shared_ptr<ice::IceSession> create_session(
            const std::string& peer, bool controlling) {
        {
            std::lock_guard lk(mu_);
            if (sessions_.count(peer)) return nullptr;
        }

        ice::IceSession::Callbacks cbs{
            .on_gathered  = [this](auto s) { on_session_gathered(s); },
            .on_connected = [this](auto s) { on_session_connected(s); },
            .on_failed    = [this](auto s) { on_session_failed(s); },
            .on_data      = [this](auto s, auto d) { on_session_data(s, d); },
        };

        auto s = std::make_shared<ice::IceSession>(
            io_, peer, controlling, ice_config_, std::move(cbs));

        {
            std::lock_guard lk(mu_);
            sessions_[peer] = s;
        }

        s->gather();
        return s;
    }

    // ── Session callbacks ───────────────────────────────────────────────────

    void on_session_gathered(std::shared_ptr<ice::IceSession> s) {
        // Сериализуем signal и отправляем
        auto signal = s->serialize_signal();
        auto kind = s->controlling() ? IceSignalKind::OFFER : IceSignalKind::ANSWER;

        send_signal(s->peer_hex(), kind, signal);
    }

    void on_session_connected(std::shared_ptr<ice::IceSession> s) {
        // Регистрируем transport path
        conn_id_t sig_conn = find_peer_conn(s->peer_hex().c_str());

        endpoint_t ep{};
        std::strncpy(ep.address, s->peer_hex().c_str(), sizeof(ep.address) - 1);
        ep.address[sizeof(ep.address) - 1] = '\0';

        conn_id_t cid;
        if (sig_conn != CONN_ID_INVALID) {
            // Есть TCP-соединение → добавляем как secondary transport
            cid = add_transport_path(s->peer_hex().c_str(), &ep, "ice");
        } else {
            // Нет TCP → primary
            cid = notify_connect(&ep);
        }

        if (cid == CONN_ID_INVALID) {
            LOG_ERROR("[ICE] failed to register connection for peer={:.8}...",
                      s->peer_hex());
            return;
        }

        s->conn_id = cid;
        LOG_INFO("[ICE] connected conn_id={} peer={:.8}...", cid, s->peer_hex());
    }

    void on_session_failed(std::shared_ptr<ice::IceSession> s) {
        LOG_WARN("[ICE] session failed peer={:.8}...", s->peer_hex());

        if (s->conn_id != CONN_ID_INVALID)
            notify_disconnect(s->conn_id, EIO);

        std::lock_guard lk(mu_);
        sessions_.erase(s->peer_hex());
    }

    void on_session_data(std::shared_ptr<ice::IceSession> s,
                          std::span<const uint8_t> data) {
        if (s->conn_id == CONN_ID_INVALID) return;
        notify_data(s->conn_id, data);
    }

    // ── Signaling ───────────────────────────────────────────────────────────

    void send_signal(const std::string& peer_hex, IceSignalKind kind,
                     const std::vector<uint8_t>& signal_data) {
        uint32_t data_len = static_cast<uint32_t>(signal_data.size());
        std::vector<uint8_t> pkt(sizeof(IceSignalHdr) + data_len);

        auto* hdr    = reinterpret_cast<IceSignalHdr*>(pkt.data());
        hdr->kind    = static_cast<uint8_t>(kind);
        std::memset(hdr->_pad, 0, sizeof(hdr->_pad));
        hdr->sdp_len = data_len;
        std::memcpy(pkt.data() + sizeof(IceSignalHdr),
                     signal_data.data(), data_len);

        conn_id_t sig_conn = find_peer_conn(peer_hex.c_str());
        if (sig_conn != CONN_ID_INVALID) {
            api_->send_response(api_->ctx, sig_conn, MSG_TYPE_ICE_SIGNAL,
                                pkt.data(), pkt.size());
        } else {
            api_->send(api_->ctx,
                       ("ice://" + peer_hex).c_str(),
                       MSG_TYPE_ICE_SIGNAL, pkt.data(), pkt.size());
        }
    }

    // ── Static signal callback ──────────────────────────────────────────────

    static void s_on_signal(void* ud, const header_t*,
                            const endpoint_t* ep,
                            const void* pl, size_t sz) {
        static_cast<IceConnector*>(ud)->on_ice_signal(
            ep, {static_cast<const uint8_t*>(pl), sz});
    }

    // ── Helpers ─────────────────────────────────────────────────────────────

    std::shared_ptr<ice::IceSession> by_peer(const std::string& h) {
        std::lock_guard lk(mu_);
        auto it = sessions_.find(h);
        return it != sessions_.end() ? it->second : nullptr;
    }

    std::shared_ptr<ice::IceSession> by_conn(conn_id_t id) {
        std::lock_guard lk(mu_);
        for (auto& [k, v] : sessions_)
            if (v->conn_id == id) return v;
        return nullptr;
    }

    static std::string hex32(const uint8_t* pk) {
        std::string out(64, '\0');
        for (int i = 0; i < 32; ++i)
            std::snprintf(&out[i * 2], 3, "%02x", pk[i]);
        return out;
    }

    // ── Members ─────────────────────────────────────────────────────────────

    asio::io_context io_;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work_;
    std::vector<std::thread> io_threads_;

    std::mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<ice::IceSession>> sessions_;

    ice::IceConfig ice_config_;

    handler_t sig_handler_{};
    static constexpr uint32_t kSigType = MSG_TYPE_ICE_SIGNAL;
};

} // namespace gn

CONNECTOR_PLUGIN(gn::IceConnector)
