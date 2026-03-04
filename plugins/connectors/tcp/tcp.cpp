#include <boost/asio.hpp>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <atomic>
#include <vector>
#include <mutex>

#include <connector.hpp>
#include <plugin.hpp>
#include <logger.hpp>

using boost::asio::ip::tcp;

namespace gn {

/**
 * Внутренний класс для управления состоянием сокета.
 * SDK не требует наследования от IConnection, мы используем его для удобства TcpConnector.
 */
struct Connection {
    tcp::socket socket;
    conn_id_t id;
    endpoint_t remote_ep;
    char read_buf[65536];

    explicit Connection(tcp::socket sock, conn_id_t cid) 
        : socket(std::move(sock)), id(cid) {
        std::memset(&remote_ep, 0, sizeof(remote_ep));
    }
};

class TcpConnector : public IConnector {
public:
    TcpConnector() = default;

    std::string get_scheme() const override { return "tcp"; }
    std::string get_name()   const override { return "GoodNet Boost.Asio TCP"; }

    void on_init() override {
        LOG_INFO("[TCP] API addr: {}, on_connect addr: {}", 
             (void*)api_, (api_ ? (void*)api_->on_connect : nullptr));
        work_.emplace(boost::asio::make_work_guard(io_));
        thread_ = std::thread([this] { io_.run(); });
        LOG_INFO("[TCP] connector ready");
    }

    void on_shutdown() override {
        LOG_INFO("[TCP] shutdown");
        if (acceptor_) acceptor_->close();
        
        work_.reset();
        io_.stop();

        std::lock_guard lock(conn_mutex_);
        for (auto& [id, conn] : connections_) {
            boost::system::error_code ec;
            conn->socket.close(ec);
        }
        connections_.clear();

        if (thread_.joinable()) thread_.join();
    }

    // --- Реализация виртуальных методов IConnector (do_...) ---

    int do_connect(const char* uri_ptr) override {
        std::string uri(uri_ptr);
        std::string target = uri;
        if (auto p = target.find("://"); p != std::string::npos)
            target = target.substr(p + 3);

        auto colon = target.rfind(':');
        if (colon == std::string::npos) return -1;

        std::string host = target.substr(0, colon);
        std::string port = target.substr(colon + 1);

        try {
            tcp::resolver resolver(io_);
            auto endpoints = resolver.resolve(host, port);
            tcp::socket sock(io_);
            boost::asio::connect(sock, endpoints);

            handle_new_socket(std::move(sock));
            return 0;
        } catch (const std::exception& e) {
            LOG_ERROR("[TCP] connect error: {}", e.what());
            return -1;
        }
    }

    int do_listen(const char* host, uint16_t port) override {
        try {
            tcp::endpoint ep(boost::asio::ip::make_address(host), port);
            acceptor_ = std::make_unique<tcp::acceptor>(io_, ep);
            do_accept();
            return 0;
        } catch (const std::exception& e) {
            LOG_ERROR("[TCP] listen error: {}", e.what());
            return -1;
        }
    }

    int do_send_to(conn_id_t id, const void* data, size_t size) override {
        std::lock_guard lock(conn_mutex_);
        auto it = connections_.find(id);
        if (it == connections_.end()) return -1;

        boost::system::error_code ec;
        boost::asio::write(it->second->socket, boost::asio::buffer(data, size), ec);
        return ec ? -1 : 0;
    }

    void do_close(conn_id_t id) override {
        std::lock_guard lock(conn_mutex_);
        if (auto it = connections_.find(id); it != connections_.end()) {
            boost::system::error_code ec;
            it->second->socket.close(ec);
            connections_.erase(it);
        }
    }

private:
    void do_accept() {
        acceptor_->async_accept([this](boost::system::error_code ec, tcp::socket sock) {
            if (!ec) handle_new_socket(std::move(sock));
            if (acceptor_->is_open()) do_accept();
        });
    }

    void handle_new_socket(tcp::socket sock) {
        endpoint_t ep{};
        try {
            auto rep = sock.remote_endpoint();
            std::strncpy(ep.address, rep.address().to_string().c_str(), sizeof(ep.address)-1);
            ep.port = rep.port();
        } catch(...) {}

        // 1. Уведомляем ядро и получаем ID
        conn_id_t id = notify_connect(&ep);
        if (id == CONN_ID_INVALID) {
            sock.close();
            return;
        }

        // 2. Сохраняем и запускаем чтение
        auto conn = std::make_shared<Connection>(std::move(sock), id);
        conn->remote_ep = ep;
        
        {
            std::lock_guard lock(conn_mutex_);
            connections_[id] = conn;
        }
        
        start_read(conn);
    }

    void start_read(std::shared_ptr<Connection> conn) {
        conn->socket.async_read_some(boost::asio::buffer(conn->read_buf),
            [this, conn](boost::system::error_code ec, std::size_t n) {
                if (ec) {
                    notify_disconnect(conn->id, ec.value());
                    std::lock_guard lock(conn_mutex_);
                    connections_.erase(conn->id);
                } else {
                    notify_data(conn->id, conn->read_buf, n);
                    start_read(conn);
                }
            });
    }

    boost::asio::io_context io_;
    std::optional<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work_;
    std::unique_ptr<tcp::acceptor> acceptor_;
    std::thread thread_;

    std::mutex conn_mutex_;
    std::unordered_map<conn_id_t, std::shared_ptr<Connection>> connections_;
};

} // namespace gn

CONNECTOR_PLUGIN(gn::TcpConnector)
