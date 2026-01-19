#include <boost/asio.hpp>
#include <iostream>
#include <thread>
#include <memory>
#include <string>

#include <connector.hpp>
#include <plugin.hpp>

using boost::asio::ip::tcp;

using namespace gn;

class TcpConnection : public IConnection, public std::enable_shared_from_this<TcpConnection> {
private:
    tcp::socket socket_;
    endpoint_t remote_ep_;
    std::string uri_;
    char read_buffer_[8192];

public:
    TcpConnection(tcp::socket socket) 
        : socket_(std::move(socket)) {
        
        try {
            auto ep = socket_.remote_endpoint();
            strncpy(remote_ep_.address, ep.address().to_string().c_str(), sizeof(remote_ep_.address) - 1);
            remote_ep_.port = ep.port();
            uri_ = "tcp://" + std::string(remote_ep_.address) + ":" + std::to_string(remote_ep_.port);
        } catch (...) {
            uri_ = "tcp://unknown";
        }
    }

    // Запуск цикла чтения
    void start() {
        do_read();
    }

    bool do_send(const void* data, size_t size) override {
        if (!socket_.is_open()) return false;
        try {
            // Синхронная отправка для простоты, либо можно сделать async_write
            boost::asio::write(socket_, boost::asio::buffer(data, size));
            return true;
        } catch (const boost::system::system_error& e) {
            notify_error(e.code().value());
            return false;
        }
    }

    void do_close() override {
        boost::system::error_code ec;
        socket_.shutdown(tcp::socket::shutdown_both, ec);
        socket_.close(ec);
        notify_close();
    }

    bool is_connected() const override {
        return socket_.is_open();
    }

    endpoint_t get_remote_endpoint() const override {
        return remote_ep_;
    }

    std::string get_uri_string() const override {
        return uri_;
    }

private:
    void do_read() {
        auto self = shared_from_this();
        socket_.async_read_some(boost::asio::buffer(read_buffer_),
            [this, self](boost::system::error_code ec, std::size_t length) {
                if (!ec) {
                    notify_data(read_buffer_, length);
                    do_read(); // Продолжаем чтение
                } else {
                    if (ec != boost::asio::error::operation_aborted) {
                        notify_error(ec.value());
                        do_close();
                    }
                }
            });
    }
};

class TcpConnector : public IConnector {
private:
    boost::asio::io_context io_context_;
    std::unique_ptr<tcp::acceptor> acceptor_;
    std::thread io_thread_;
    std::vector<std::shared_ptr<TcpConnection>> active_connections_; // Для контроля жизненного цикла

public:
    TcpConnector() = default;

    ~TcpConnector() override {
        on_shutdown();
    }

    std::string get_scheme() const override { return "tcp"; }
    std::string get_name() const override { return "Boost Asio TCP Connector"; }

    void on_init() override {
        // Запускаем io_context в отдельном потоке
        io_thread_ = std::thread([this]() {
            auto work = boost::asio::make_work_guard(io_context_);
            io_context_.run();
        });
    }

    void on_shutdown() override {
        io_context_.stop();
        if (io_thread_.joinable()) {
            io_thread_.join();
        }
        if (acceptor_) {
            acceptor_->close();
        }
    }

    std::unique_ptr<IConnection> create_connection(const std::string& uri) override {
        try {
            // Парсинг простого формата "host:port" или "tcp://host:port"
            std::string target = uri;
            if (target.find("://") != std::string::npos) {
                target = target.substr(target.find("://") + 3);
            }

            size_t colon_pos = target.find(':');
            if (colon_pos == std::string::npos) return nullptr;

            std::string host = target.substr(0, colon_pos);
            std::string port = target.substr(colon_pos + 1);

            tcp::resolver resolver(io_context_);
            auto endpoints = resolver.resolve(host, port);

            tcp::socket socket(io_context_);
            boost::asio::connect(socket, endpoints);

            auto conn = std::make_shared<TcpConnection>(std::move(socket));
            conn->start();
            
            // Возвращаем raw ptr через unique_ptr (как требует SDK)
            // Но используем shared_ptr внутри для асинхронных операций
            return std::unique_ptr<IConnection>(conn.get()); 
            // Внимание: В реальной системе нужно продумать владение, 
            // чтобы shared_ptr не удалился раньше времени.
        } catch (const std::exception& e) {
            std::cerr << "[TCP] Connect error: " << e.what() << std::endl;
            return nullptr;
        }
    }

    bool start_listening(const std::string& host, uint16_t port) override {
        try {
            tcp::endpoint endpoint(boost::asio::ip::make_address(host), port);
            acceptor_ = std::make_unique<tcp::acceptor>(io_context_, endpoint);
            
            do_accept();
            return true;
        } catch (const std::exception& e) {
            std::cerr << "[TCP] Listen error: " << e.what() << std::endl;
            return false;
        }
    }

private:
    void do_accept() {
        acceptor_->async_accept([this](boost::system::error_code ec, tcp::socket socket) {
            if (!ec) {
                auto conn = std::make_shared<TcpConnection>(std::move(socket));
                conn->start();

                // Здесь нужно уведомить ядро о новом соединении.
                // В вашем SDK обычно это делается через api_->on_connect или похожий механизм.
                // Если ядро ожидает уведомления через send:
                std::string uri = conn->get_uri_string();
                this->send(uri.c_str(), MSG_TYPE_SYSTEM, nullptr, 0); 
                
                // Сохраняем в список активных, чтобы shared_ptr жил
                active_connections_.push_back(conn);
            }
            do_accept();
        });
    }
};

// Экспорт плагина
CONNECTOR_PLUGIN(TcpConnector)
