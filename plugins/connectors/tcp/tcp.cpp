#include <boost/asio.hpp>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <../sdk/cpp/data.hpp>
#include <connector.hpp>
#include <logger.hpp>

using namespace gn::sdk;
using boost::asio::ip::tcp;
namespace asio = boost::asio;

namespace gn {

// ─── TcpConnection ────────────────────────────────────────────────────────────

static constexpr size_t HDR_SIZE    = sizeof(header_t);
static constexpr size_t MAX_PAYLOAD = 64UL * 1024 * 1024;  // 64 MB санитарный лимит

struct TcpConnection {
    tcp::socket  socket;
    conn_id_t    id;
    endpoint_t   remote;

    // ── Чтение (serial: header→body→header…, только из io-потоков) ───────────
    std::array<uint8_t, HDR_SIZE> hdr_buf{};
    // body_buf: capacity растёт до максимального пакета и остаётся — нет malloc после warmup
    std::vector<uint8_t> body_buf;

    // ── Запись (защищена write_mu) ────────────────────────────────────────────
    std::mutex                       write_mu;
    std::deque<std::vector<uint8_t>> write_queue;
    bool                             writing = false;

    TcpConnection(tcp::socket s, conn_id_t cid, const endpoint_t& ep)
        : socket(std::move(s)), id(cid), remote(ep)
    {}

    TcpConnection(const TcpConnection&)            = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;
};

// ─── TcpConnector ─────────────────────────────────────────────────────────────

class TcpConnector : public IConnector {
public:
    TcpConnector()  = default;
    ~TcpConnector() = default;

    std::string get_scheme() const override { return "tcp"; }
    std::string get_name()   const override { return "GoodNet Boost.Asio TCP"; }

    // ─── Жизненный цикл ──────────────────────────────────────────────────────

    void on_init() override {
        work_.emplace(asio::make_work_guard(io_));
        const int n = std::max(2, (int)std::thread::hardware_concurrency());
        io_threads_.reserve(n);
        for (int i = 0; i < n; ++i)
            io_threads_.emplace_back([this] {
                LOG_DEBUG("[TCP] io_thread started");
                io_.run();
            });

        LOG_INFO("[TCP] connector ready ({} io threads)", n);
    }

    void on_shutdown() override {
        LOG_INFO("[TCP] shutting down...");

        asio::post(io_, [this] {
            if (acceptor_ && acceptor_->is_open()) {
                boost::system::error_code ec;
                acceptor_->close(ec);
            }
            std::lock_guard lock(conn_mu_);
            for (auto& [id, conn] : connections_) {
                boost::system::error_code ec;
                conn->socket.close(ec);
            }
            connections_.clear();
        });

        work_.reset();
        io_.stop();
        for (auto& t : io_threads_)
            if (t.joinable()) t.join();
        LOG_INFO("[TCP] shutdown complete");
    }

    // ─── Исходящее подключение ────────────────────────────────────────────────

    int do_connect(const char* uri_cstr) override {
        std::string target(uri_cstr);
        if (const auto p = target.find("://"); p != std::string::npos)
            target = target.substr(p + 3);

        const auto colon = target.rfind(':');
        if (colon == std::string::npos) {
            LOG_ERROR("[TCP] Invalid URI: {}", uri_cstr);
            return -1;
        }
        std::string host = target.substr(0, colon);
        std::string port = target.substr(colon + 1);

        asio::post(io_, [this, h = std::move(host), p = std::move(port)]() mutable {
            auto resolver = std::make_shared<tcp::resolver>(io_);
            resolver->async_resolve(h, p,
                [this, resolver, h, p](auto ec, tcp::resolver::results_type eps) {
                    if (ec) {
                        LOG_ERROR("[TCP] Resolve failed {}:{}: {}", h, p, ec.message());
                        return;
                    }
                    auto sock = std::make_shared<tcp::socket>(io_);
                    asio::async_connect(*sock, eps,
                        [this, sock, h, p](auto ec2, const tcp::endpoint&) {
                            if (ec2) {
                                LOG_ERROR("[TCP] Connect failed {}:{}: {}", h, p, ec2.message());
                                return;
                            }
                            LOG_INFO("[TCP] Connected to {}:{}", h, p);
                            register_socket(std::move(*sock));
                        });
                });
        });
        return 0;
    }

    // ─── Сервер ───────────────────────────────────────────────────────────────

    int do_listen(const char* host_str, uint16_t port) override {
        auto prom = std::make_shared<std::promise<int>>();
        auto fut  = prom->get_future();

        asio::post(io_, [this, host = std::string(host_str), port, prom]() mutable {
            try {
                tcp::endpoint ep(asio::ip::make_address(host), port);
                acceptor_ = std::make_unique<tcp::acceptor>(io_);
                acceptor_->open(ep.protocol());
                acceptor_->set_option(tcp::acceptor::reuse_address(true));
                acceptor_->bind(ep);
                acceptor_->listen();
                LOG_INFO("[TCP] Listening on {}:{}", host, port);
                accept_next();
                prom->set_value(0);
            } catch (const std::exception& e) {
                LOG_ERROR("[TCP] Listen failed {}:{}: {}", host, port, e.what());
                prom->set_value(-1);
            }
        });
        return fut.get();
    }

    // ─── Отправка данных ──────────────────────────────────────────────────────

    int do_send(conn_id_t id, std::span<const uint8_t> data) override {
        // Копируем данные в вектор для асинхронной передачи
        auto buf = std::make_shared<std::vector<uint8_t>>(data.begin(), data.end());

        asio::post(io_, [this, id, buf]() {
            std::shared_ptr<TcpConnection> conn;
            {
                std::lock_guard lock(conn_mu_);
                auto it = connections_.find(id);
                if (it == connections_.end()) return;
                conn = it->second;
            }

            bool should_start = false;
            {
                std::lock_guard lk(conn->write_mu);
                conn->write_queue.push_back(std::move(*buf));
                if (!conn->writing) {
                    conn->writing = true;
                    should_start  = true;
                }
            }
            if (should_start) start_write(std::move(conn));
        });
        return 0;
    }

    void do_close(conn_id_t id, bool hard) override {
        asio::post(io_, [this, id, hard] {
            std::lock_guard lock(conn_mu_);
            if (auto it = connections_.find(id); it != connections_.end()) {
                boost::system::error_code ec;
                if (hard) {
                    it->second->socket.lowest_layer().cancel(); // Отмена всех операций
                }
                it->second->socket.close(ec);
                connections_.erase(it);
            }
        });
    }

private:
    // ─── Accept ───────────────────────────────────────────────────────────────

    void accept_next() {
        if (!acceptor_ || !acceptor_->is_open()) return;
        acceptor_->async_accept([this](auto ec, tcp::socket sock) {
            if (!ec) {
                register_socket(std::move(sock));
            } else if (ec != asio::error::operation_aborted) {
                LOG_WARN("[TCP] Accept error: {}", ec.message());
            }
            accept_next();
        });
    }

    void register_socket(tcp::socket sock) {
        endpoint_t ep{};
        try {
            auto rep = sock.remote_endpoint();
            std::strncpy(ep.address, rep.address().to_string().c_str(),
                         sizeof(ep.address) - 1);
            ep.port = rep.port();
        } catch (...) {}

        conn_id_t id = notify_connect(&ep);
        if (id == CONN_ID_INVALID) {
            LOG_WARN("[TCP] Core rejected connection from {}", ep.address);
            sock.close();
            return;
        }

        auto conn = std::make_shared<TcpConnection>(std::move(sock), id, ep);
        {
            std::lock_guard lock(conn_mu_);
            connections_[id] = conn;
        }
        LOG_INFO("[TCP] Registered #{} ({}:{})", id, ep.address, ep.port);
        start_read_header(std::move(conn));
    }

    // ─── Двухфазное framed чтение ─────────────────────────────────────────────

    void start_read_header(std::shared_ptr<TcpConnection> conn) {
        // async_read гарантирует ровно HDR_SIZE байт — никаких обрывков
        asio::async_read(conn->socket,
            asio::buffer(conn->hdr_buf.data(), HDR_SIZE),
            [this, conn](auto ec, std::size_t) mutable {
                if (ec) { on_read_error(conn, ec); return; }

                const auto* hdr = reinterpret_cast<const header_t*>(
                    conn->hdr_buf.data());

                // Ранняя валидация — до аллокации body
                if (hdr->magic != GNET_MAGIC) {
                    LOG_WARN("[TCP] #{}: bad magic 0x{:08X}, closing",
                             conn->id, hdr->magic);
                    on_protocol_error(conn);
                    return;
                }
                if (hdr->proto_ver != GNET_PROTO_VER) {
                    LOG_WARN("[TCP] #{}: bad proto_ver {}, closing",
                             conn->id, hdr->proto_ver);
                    on_protocol_error(conn);
                    return;
                }

                const size_t plen = hdr->payload_len;

                // Пустое тело (MSG_TYPE_SYSTEM keepalive / пустой AUTH etc)
                if (plen == 0) {
                    LOG_TRACE("[TCP] #{}: header-only frame type={}",
                              conn->id, hdr->payload_type);
                    notify_data(conn->id, RawSpan(conn->hdr_buf.data(), HDR_SIZE));
                    start_read_header(std::move(conn));
                    return;
                }

                if (plen > MAX_PAYLOAD) {
                    LOG_WARN("[TCP] #{}: payload_len {} > {} (MAX), closing",
                             conn->id, plen, MAX_PAYLOAD);
                    on_protocol_error(conn);
                    return;
                }

                // resize сохраняет capacity — malloc только если plen > старый capacity
                conn->body_buf.resize(plen);
                start_read_body(std::move(conn), plen);
            });
    }

    void start_read_body(std::shared_ptr<TcpConnection> conn, size_t plen) {
        asio::async_read(conn->socket,
            asio::buffer(conn->body_buf.data(), plen),
            [this, conn, plen](auto ec, std::size_t) mutable {
                if (ec) { on_read_error(conn, ec); return; }

                // Сборка полного фрейма в thread_local буфер:
                // одна аллокация на поток (при первом пакете), потом только resize.
                thread_local std::vector<uint8_t> tl_frame;
                const size_t total = HDR_SIZE + plen;
                tl_frame.resize(total);
                std::memcpy(tl_frame.data(),            conn->hdr_buf.data(), HDR_SIZE);
                std::memcpy(tl_frame.data() + HDR_SIZE, conn->body_buf.data(), plen);

                LOG_TRACE("[TCP] #{}: complete frame {} bytes", conn->id, total);

                // handle_data всегда видит ровно один полный фрейм →
                // recv_buf никогда не накапливает частичные данные →
                // decrypt никогда не получает обрывки → decrypt_fail = 0
                notify_data(conn->id, RawSpan(tl_frame.data(), total));

                start_read_header(std::move(conn));
            });
    }

    void on_read_error(std::shared_ptr<TcpConnection>& conn,
                       const boost::system::error_code& ec) {
        const int err = (ec == asio::error::eof ||
                         ec == asio::error::operation_aborted) ? 0 : ec.value();
        LOG_INFO("[TCP] #{} read closed: {}", conn->id, ec.message());
        notify_disconnect(conn->id, err);
        std::lock_guard lock(conn_mu_);
        connections_.erase(conn->id);
    }

    void on_protocol_error(std::shared_ptr<TcpConnection>& conn) {
        boost::system::error_code ec;
        conn->socket.close(ec);
        notify_disconnect(conn->id, 1);
        std::lock_guard lock(conn_mu_);
        connections_.erase(conn->id);
    }

    // ─── Write pipeline ───────────────────────────────────────────────────────

    void start_write(std::shared_ptr<TcpConnection> conn) {
        std::shared_ptr<std::vector<uint8_t>> frame;
        {
            std::lock_guard lk(conn->write_mu);
            if (conn->write_queue.empty()) {
                conn->writing = false;
                return;
            }
            frame = std::make_shared<std::vector<uint8_t>>(
                std::move(conn->write_queue.front()));
            conn->write_queue.pop_front();
        }

        asio::async_write(conn->socket, asio::buffer(*frame),
            [this, conn, frame](auto ec, std::size_t n) {
                if (ec && ec != asio::error::operation_aborted) {
                    LOG_WARN("[TCP] #{} write error: {}", conn->id, ec.message());
                    std::lock_guard lk(conn->write_mu);
                    conn->write_queue.clear();
                    conn->writing = false;
                    return;
                }
                LOG_TRACE("[TCP] #{} wrote {} bytes", conn->id, n);

                bool more = false;
                {
                    std::lock_guard lk(conn->write_mu);
                    more = !conn->write_queue.empty();
                    if (!more) conn->writing = false;
                }
                if (more) start_write(std::move(conn));
            });
    }

    // ─── Состояние ───────────────────────────────────────────────────────────

    asio::io_context io_;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work_;
    std::unique_ptr<tcp::acceptor> acceptor_;
    std::vector<std::thread> io_threads_;

    std::mutex conn_mu_;
    std::unordered_map<conn_id_t, std::shared_ptr<TcpConnection>> connections_;
};

} // namespace gn

// Регистрация плагина
CONNECTOR_PLUGIN(gn::TcpConnector)