#include <boost/asio.hpp>
#include <atomic>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <connector.hpp>
#include <plugin.hpp>
#include <logger.hpp>

using boost::asio::ip::tcp;
namespace gn {

// ─── TcpConnection ────────────────────────────────────────────────────────────
//
// Ownership model:
//   • Outgoing (client-side): created by create_connection(), released via
//     unique_ptr into to_c_ops(). TcpConnector also keeps a shared_ptr in
//     connections_ so the object lives as long as the connector lives OR the
//     caller explicitly closes it.
//   • Incoming (server-side): kept exclusively in connections_. The connector
//     owns it. No unique_ptr is handed outside — the C handle stays in
//     connection_ops_t embedded inside the object itself.

class TcpConnection : public IConnection,
                      public std::enable_shared_from_this<TcpConnection> {
public:
    // Passed in by do_accept so incoming data reaches the core.
    // For outgoing connections the caller sets this via set_callbacks().
    explicit TcpConnection(tcp::socket socket, host_api_t* api = nullptr)
        : socket_(std::move(socket)), api_(api)
    {
        try {
            auto ep = socket_.remote_endpoint();
            std::string addr = ep.address().to_string();
            std::strncpy(remote_ep_.address, addr.c_str(),
                         sizeof(remote_ep_.address) - 1);
            remote_ep_.port = ep.port();
            uri_ = "tcp://" + addr + ":" + std::to_string(ep.port());
        } catch (...) {
            uri_ = "tcp://unknown";
        }
    }

    void start() { do_read(); }

    // ── IConnection ────────────────────────────────────────────────────────────

    bool do_send(const void* data, size_t size) override {
        if (!socket_.is_open()) return false;
        boost::system::error_code ec;
        boost::asio::write(socket_, boost::asio::buffer(data, size), ec);
        if (ec) { notify_error(ec.value()); return false; }
        return true;
    }

    void do_close() override {
        if (closed_.exchange(true)) return;
        boost::system::error_code ec;
        socket_.shutdown(tcp::socket::shutdown_both, ec);
        socket_.close(ec);
        notify_close();
    }

    bool        is_connected() const        override { return socket_.is_open(); }
    endpoint_t  get_remote_endpoint() const override { return remote_ep_; }
    std::string get_uri_string() const      override { return uri_; }

private:
    void do_read() {
        auto self = shared_from_this();
        socket_.async_read_some(
            boost::asio::buffer(read_buf_),
            [this, self](boost::system::error_code ec, std::size_t n) {
                if (ec) { do_close(); return; }
                // Route to whoever registered a callback (set_callbacks).
                // For server-side connections api_ is set at construction,
                // so we forward raw bytes directly through api_->send.
                // The core's api.send handler decides what to do with them.
                notify_data(read_buf_, n);
                do_read();
            });
    }

    tcp::socket        socket_;
    endpoint_t         remote_ep_{};
    std::string        uri_;
    char               read_buf_[65536];
    std::atomic<bool>  closed_{false};
    host_api_t*        api_ = nullptr;  // non-owning, set for server connections
};

// ─── TcpConnector ─────────────────────────────────────────────────────────────

class TcpConnector : public IConnector {
public:
    std::string get_scheme() const override { return "tcp"; }
    std::string get_name()   const override { return "GoodNet Boost.Asio TCP Connector"; }

    void on_init() override {
        work_guard_.emplace(io_context_.get_executor());
        io_thread_ = std::thread([this] { io_context_.run(); });
        LOG_INFO("[TCP] Connector initialized");
    }

    void on_shutdown() override {
        LOG_INFO("[TCP] Shutting down");
        if (acceptor_) { acceptor_->close(); }
        io_context_.stop();
        if (io_thread_.joinable()) io_thread_.join();
        std::lock_guard lock(conn_mutex_);
        for (auto& c : connections_) c->do_close();
        connections_.clear();
    }

    // ── Outgoing connection ───────────────────────────────────────────────────

    std::unique_ptr<IConnection> create_connection(const std::string& uri) override {
        try {
            std::string target = uri;
            if (auto p = target.find("://"); p != std::string::npos)
                target = target.substr(p + 3);

            auto colon = target.find(':');
            if (colon == std::string::npos) {
                LOG_ERROR("[TCP] Bad URI: {}", uri);
                return nullptr;
            }
            std::string host = target.substr(0, colon);
            std::string port = target.substr(colon + 1);

            LOG_INFO("[TCP] Connecting to {}:{}", host, port);

            tcp::resolver resolver(io_context_);
            auto eps = resolver.resolve(host, port);
            tcp::socket sock(io_context_);
            boost::asio::connect(sock, eps);

            LOG_INFO("[TCP] Connected: {}", uri);

            // Build a shared_ptr that we keep in connections_ for lifetime,
            // AND hand out a non-owning raw pointer via unique_ptr with a
            // no-op deleter so to_c_ops() release() doesn't double-free.
            auto conn = std::make_shared<TcpConnection>(std::move(sock), api_);
            {
                std::lock_guard lock(conn_mutex_);
                connections_.push_back(conn);
            }
            conn->start();

            // Return unique_ptr with no-op deleter.
            // Lifetime is controlled by connections_ inside TcpConnector.
            return std::unique_ptr<IConnection>(
                conn.get(),
                [](IConnection*) noexcept {} // no-op: shared_ptr owns it
            );

        } catch (const std::exception& e) {
            LOG_ERROR("[TCP] connect error: {}", e.what());
            return nullptr;
        }
    }

    // ── Incoming connections ──────────────────────────────────────────────────

    bool start_listening(const std::string& host, uint16_t port) override {
        try {
            tcp::endpoint ep(boost::asio::ip::make_address(host), port);
            acceptor_ = std::make_unique<tcp::acceptor>(io_context_, ep);
            LOG_INFO("[TCP] Listening on {}:{}", host, port);
            do_accept();
            return true;
        } catch (const std::exception& e) {
            LOG_ERROR("[TCP] listen error: {}", e.what());
            return false;
        }
    }

private:
    void do_accept() {
        acceptor_->async_accept(
            [this](boost::system::error_code ec, tcp::socket sock) {
                if (ec) {
                    // acceptor was closed (shutdown) — stop the chain
                    if (ec != boost::asio::error::operation_aborted)
                        LOG_ERROR("[TCP] accept error: {}", ec.message());
                    return;
                }

                // Build the connection; give it api_ so server-side data
                // can be routed to the core via callbacks later.
                auto conn = std::make_shared<TcpConnection>(std::move(sock), api_);
                const std::string uri = conn->get_uri_string();
                LOG_INFO("[TCP] Incoming connection from {}", uri);

                // Wire incoming data → api_->send so the core's handlers
                // receive the bytes. This is the server-side data path.
                // We use a per-connection callbacks struct stored inside
                // the lambda's closure (heap-allocated, lives as long as conn).
                auto* cbs_ptr = new connection_callbacks_t{};
                cbs_ptr->user_data = api_;
                cbs_ptr->on_data = [](void* ud, const void* data, size_t size) {
                    auto* a = static_cast<host_api_t*>(ud);
                    if (a && a->send) a->send("tcp://incoming", MSG_TYPE_CHAT,
                                              data, size);
                };
                cbs_ptr->on_close = [](void* ud) {
                    // optional: log disconnect
                    (void)ud;
                };
                cbs_ptr->on_error = [](void* ud, int code) {
                    (void)ud; (void)code;
                };

                // set_callbacks copies the struct, so we can delete cbs_ptr
                auto* ops = conn->to_c_ops();
                ops->set_callbacks(ops->conn_ctx, cbs_ptr);
                delete cbs_ptr;

                {
                    std::lock_guard lock(conn_mutex_);
                    connections_.push_back(conn);
                }
                conn->start();

                if (api_ && api_->update_connection_state)
                    api_->update_connection_state(uri.c_str(), STATE_ESTABLISHED);

                // Keep accepting
                do_accept();
            });
    }

    boost::asio::io_context io_context_;
    std::optional<boost::asio::executor_work_guard<
        boost::asio::io_context::executor_type>> work_guard_;
    std::unique_ptr<tcp::acceptor> acceptor_;
    std::thread io_thread_;

    std::list<std::shared_ptr<TcpConnection>> connections_;
    std::mutex conn_mutex_;
};

} // namespace gn

CONNECTOR_PLUGIN(gn::TcpConnector)
