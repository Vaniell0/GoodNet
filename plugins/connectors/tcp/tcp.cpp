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
static constexpr size_t MAX_PAYLOAD = 64UL * 1024 * 1024;  ///< 64 MB sanity limit

/// @brief Per-connection state for the TCP transport.
struct TcpConnection {
    tcp::socket  socket;
    conn_id_t    id;
    endpoint_t   remote;

    /// Contiguous frame buffer: [header][payload].
    /// Capacity grows to max frame size and stays (zero-alloc after warmup).
    std::vector<uint8_t> frame_buf;

    // ── Write state (guarded by write_mu) ────────────────────────────────────
    std::mutex                       write_mu;
    std::deque<std::vector<uint8_t>> write_queue;
    bool                             writing = false;

    TcpConnection(tcp::socket s, conn_id_t cid, const endpoint_t& ep)
        : socket(std::move(s)), id(cid), remote(ep)
    {
        frame_buf.resize(HDR_SIZE);
    }

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

    // ─── Lifecycle ────────────────────────────────────────────────────────────

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

    // ─── Outbound connection ────────────────────────────────────────────────

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

    // ─── Server (accept) ──────────────────────────────────────────────────────

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

    // ─── Send ────────────────────────────────────────────────────────────────

    int do_send(conn_id_t id, std::span<const uint8_t> data) override {
        // Copy data into a shared buffer for async transmission.
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

    /// @brief Close a connection and notify the core.
    /// @param hard  true = cancel pending I/O immediately, false = graceful.
    void do_close(conn_id_t id, bool hard) override {
        asio::post(io_, [this, id, hard] {
            {
                std::lock_guard lock(conn_mu_);
                auto it = connections_.find(id);
                if (it == connections_.end()) {
                    // Connection already removed — still notify core so it cleans up.
                    notify_disconnect(id, 0);
                    return;
                }
                boost::system::error_code ec;
                if (hard)
                    it->second->socket.lowest_layer().cancel(ec);
                it->second->socket.close(ec);
                connections_.erase(it);
            }
            // Notify core so it removes the ConnectionRecord from the RCU map.
            notify_disconnect(id, 0);
        });
    }

    // ─── Scatter-gather send ─────────────────────────────────────────────────

    /// @brief Batch-enqueue all iov segments at once, avoiding per-frame do_send() loop.
    int do_send_gather(conn_id_t id, const struct iovec* iov, int n) override {
        auto batch = std::make_shared<std::vector<std::vector<uint8_t>>>();
        batch->reserve(n);
        for (int i = 0; i < n; ++i)
            batch->emplace_back(
                static_cast<const uint8_t*>(iov[i].iov_base),
                static_cast<const uint8_t*>(iov[i].iov_base) + iov[i].iov_len);

        asio::post(io_, [this, id, batch]() {
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
                for (auto& f : *batch)
                    conn->write_queue.push_back(std::move(f));
                if (!conn->writing) {
                    conn->writing = true;
                    should_start = true;
                }
            }
            if (should_start) start_write(std::move(conn));
        });
        return 0;
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
            if (rep.address().is_loopback())
                ep.flags = EP_FLAG_TRUSTED;
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

    // ─── Two-phase framed reading (zero-copy) ───────────────────────────────────

    void start_read_header(std::shared_ptr<TcpConnection> conn) {
        conn->frame_buf.resize(HDR_SIZE);  // shrink size, keep capacity
        asio::async_read(conn->socket,
            asio::buffer(conn->frame_buf.data(), HDR_SIZE),
            [this, conn](auto ec, std::size_t) mutable {
                if (ec) { on_read_error(conn, ec); return; }

                const auto* hdr = reinterpret_cast<const header_t*>(
                    conn->frame_buf.data());

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

                if (plen == 0) {
                    LOG_TRACE("[TCP] #{}: header-only frame type={}",
                              conn->id, hdr->payload_type);
                    notify_data(conn->id, RawSpan(conn->frame_buf.data(), HDR_SIZE));
                    start_read_header(std::move(conn));
                    return;
                }

                if (plen > MAX_PAYLOAD) {
                    LOG_WARN("[TCP] #{}: payload_len {} > {} (MAX), closing",
                             conn->id, plen, MAX_PAYLOAD);
                    on_protocol_error(conn);
                    return;
                }

                conn->frame_buf.resize(HDR_SIZE + plen);  // preserves header bytes
                start_read_body(std::move(conn), plen);
            });
    }

    void start_read_body(std::shared_ptr<TcpConnection> conn, size_t plen) {
        asio::async_read(conn->socket,
            asio::buffer(conn->frame_buf.data() + HDR_SIZE, plen),
            [this, conn](auto ec, std::size_t) mutable {
                if (ec) { on_read_error(conn, ec); return; }

                const size_t total = conn->frame_buf.size();
                LOG_TRACE("[TCP] #{}: frame {} bytes", conn->id, total);

                // Zero-copy: frame_buf is stable during synchronous notify_data.
                notify_data(conn->id, RawSpan(conn->frame_buf.data(), total));
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

    static constexpr size_t WRITE_BATCH_SIZE = 64;

    void start_write(std::shared_ptr<TcpConnection> conn) {
        // Drain up to WRITE_BATCH_SIZE frames into a single scatter buffer list.
        // async_write with ConstBufferSequence maps to writev() — one syscall.
        auto frames = std::make_shared<std::vector<std::vector<uint8_t>>>();
        std::vector<asio::const_buffer> buffers;
        {
            std::lock_guard lk(conn->write_mu);
            if (conn->write_queue.empty()) {
                conn->writing = false;
                return;
            }
            const size_t n = std::min(conn->write_queue.size(), WRITE_BATCH_SIZE);
            frames->reserve(n);
            buffers.reserve(n);
            for (size_t i = 0; i < n; ++i) {
                frames->push_back(std::move(conn->write_queue.front()));
                conn->write_queue.pop_front();
                auto& f = frames->back();
                buffers.emplace_back(f.data(), f.size());
            }
        }

        asio::async_write(conn->socket, buffers,
            [this, conn, frames](auto ec, std::size_t n) {
                if (ec && ec != asio::error::operation_aborted) {
                    LOG_WARN("[TCP] #{} write error: {}", conn->id, ec.message());
                    std::lock_guard lk(conn->write_mu);
                    conn->write_queue.clear();
                    conn->writing = false;
                    return;
                }
                LOG_TRACE("[TCP] #{} wrote {} bytes ({} frames)",
                          conn->id, n, frames->size());

                bool more = false;
                {
                    std::lock_guard lk(conn->write_mu);
                    more = !conn->write_queue.empty();
                    if (!more) conn->writing = false;
                }
                if (more) start_write(std::move(conn));
            });
    }

    // ─── State ───────────────────────────────────────────────────────────────

    asio::io_context io_;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work_;
    std::unique_ptr<tcp::acceptor> acceptor_;
    std::vector<std::thread> io_threads_;

    std::mutex conn_mu_;
    std::unordered_map<conn_id_t, std::shared_ptr<TcpConnection>> connections_;
};

} // namespace gn

// Plugin registration.
CONNECTOR_PLUGIN(gn::TcpConnector)