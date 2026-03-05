#include <boost/asio.hpp>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <connector.hpp>
#include <plugin.hpp>
#include <logger.hpp>

using boost::asio::ip::tcp;
namespace asio = boost::asio;

namespace gn {

/**
 * @brief Структура владения TCP соединением.
 * * ВАЖНО: Все операции с socket (async_read/write) должны происходить 
 * ТОЛЬКО в потоке io_context.
 */
struct TcpConnection {
    tcp::socket socket;
    conn_id_t   id;
    endpoint_t  remote;
    uint8_t     read_buf[65536];

    TcpConnection(tcp::socket s, conn_id_t cid, const endpoint_t& ep)
        : socket(std::move(s)), id(cid), remote(ep) {}
    
    // Запрещаем копирование
    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;
};

/**
 * @brief TcpConnector — Транспортный плагин на базе Boost.Asio.
 */
class TcpConnector : public IConnector {
public:
    TcpConnector() = default;
    virtual ~TcpConnector() = default;

    // Идентификация плагина
    std::string get_scheme() const override { return "tcp"; }
    std::string get_name()   const override { return "GoodNet Boost.Asio TCP"; }

    // ─── Жизненный цикл ──────────────────────────────────────────────────────

    void on_init() override {
        // Создаем guard, чтобы io_.run() не завершался при отсутствии задач
        work_.emplace(asio::make_work_guard(io_));
        
        // Запускаем выделенный поток для сетевых операций
        io_thread_ = std::thread([this] { 
            LOG_DEBUG("[TCP] io_thread started");
            io_.run(); 
        });
        
        LOG_INFO("[TCP] connector ready");
    }

    void on_shutdown() override {
        LOG_INFO("[TCP] shutting down...");

        // Корректно закрываем всё внутри io_thread
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

        // Останавливаем цикл событий
        work_.reset();
        io_.stop();

        if (io_thread_.joinable()) {
            io_thread_.join();
        }
        LOG_INFO("[TCP] shutdown complete");
    }

    // ─── Сетевые операции (Thread-Safe) ──────────────────────────────────────

    /**
     * @brief Исходящее подключение.
     * Вызывается из CLI/Ядра. Постит задачу в IO поток.
     */
    int do_connect(const char* uri_cstr) override {
        std::string target(uri_cstr);
        
        // Парсим URI (упрощенно)
        if (const auto p = target.find("://"); p != std::string::npos)
            target = target.substr(p + 3);

        const auto colon = target.rfind(':');
        if (colon == std::string::npos) {
            LOG_ERROR("[TCP] Invalid URI format: {}", uri_cstr);
            return -1;
        }

        std::string host = target.substr(0, colon);
        std::string port = target.substr(colon + 1);

        LOG_DEBUG("[TCP] Queuing async connect to {}:{}", host, port);

        // Передаем управление в io_thread
        asio::post(io_, [this, h = std::move(host), p = std::move(port)]() mutable {
            auto resolver = std::make_shared<tcp::resolver>(io_);
            
            resolver->async_resolve(h, p,
                [this, resolver, h, p](auto ec, tcp::resolver::results_type eps) {
                    if (ec) {
                        LOG_ERROR("[TCP] Resolve failed for {}:{}: {}", h, p, ec.message());
                        return;
                    }

                    auto sock = std::make_shared<tcp::socket>(io_);
                    asio::async_connect(*sock, eps,
                        [this, sock, h, p](auto ec2, const tcp::endpoint& /*ep*/) {
                            if (ec2) {
                                LOG_ERROR("[TCP] Connect failed to {}:{}: {}", h, p, ec2.message());
                                return;
                            }
                            LOG_INFO("[TCP] Connected to {}:{}", h, p);
                            register_socket(std::move(*sock));
                        });
                });
        });

        return 0; // Возвращаем успех запроса (само подключение идет в фоне)
    }

    /**
     * @brief Ожидание входящих соединений.
     */
    int do_listen(const char* host_str, uint16_t port) override {
        std::string host(host_str);
        auto result_promise = std::make_shared<std::promise<int>>();
        auto result_future = result_promise->get_future();

        asio::post(io_, [this, host, port, result_promise]() mutable {
            try {
                tcp::endpoint ep(asio::ip::make_address(host), port);
                acceptor_ = std::make_unique<tcp::acceptor>(io_);
                
                acceptor_->open(ep.protocol());
                // Исправляет "Address already in use" при рестартах
                acceptor_->set_option(tcp::acceptor::reuse_address(true));
                
                acceptor_->bind(ep);
                acceptor_->listen();

                LOG_INFO("[TCP] Server listening on {}:{}", host, port);
                accept_next();
                result_promise->set_value(0);
            } catch (const std::exception& e) {
                LOG_ERROR("[TCP] Listen failed on {}:{}: {}", host, port, e.what());
                result_promise->set_value(-1);
            }
        });

        return result_future.get(); // Синхронно ждем только результата бинда
    }

    /**
     * @brief Отправка данных.
     * Вызывается из любого потока. Гарантирует thread-safety через post.
     */
    int do_send_to(conn_id_t id, const void* data, size_t size) override {
        // Копируем данные в buffer для асинхронной передачи
        auto buf = std::make_shared<std::vector<uint8_t>>(
            static_cast<const uint8_t*>(data),
            static_cast<const uint8_t*>(data) + size);

        asio::post(io_, [this, id, buf]() {
            std::shared_ptr<TcpConnection> conn;
            {
                std::lock_guard lock(conn_mu_);
                auto it = connections_.find(id);
                if (it == connections_.end()) return;
                conn = it->second;
            }

            asio::async_write(conn->socket, asio::buffer(*buf),
                [id, buf](auto ec, std::size_t n) {
                    if (ec && ec != asio::error::operation_aborted) {
                        LOG_WARN("[TCP] Send error on id={}: {}", id, ec.message());
                    } else if (!ec) {
                        LOG_TRACE("[TCP] Sent {} bytes to id={}", n, id);
                    }
                });
        });
        return 0;
    }

    void do_close(conn_id_t id) override {
        asio::post(io_, [this, id]() {
            std::lock_guard lock(conn_mu_);
            if (auto it = connections_.find(id); it != connections_.end()) {
                boost::system::error_code ec;
                it->second->socket.close(ec);
                connections_.erase(it);
                LOG_DEBUG("[TCP] Connection id={} closed manually", id);
            }
        });
    }

private:
    // ─── Внутренняя логика (Выполняется в io_thread) ─────────────────────────

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
            std::strncpy(ep.address, rep.address().to_string().c_str(), sizeof(ep.address)-1);
            ep.port = rep.port();
        } catch(...) {}

        LOG_DEBUG("[TCP] New socket from {}:{}", ep.address, ep.port);

        // 1. Уведомляем ядро. 
        // Важно: так как мы в io_thread, ядро сразу вызовет send_auth, 
        // который через post встанет в очередь этого же потока.
        conn_id_t id = notify_connect(&ep);
        
        if (id == CONN_ID_INVALID) {
            LOG_WARN("[TCP] Kernel rejected connection from {}", ep.address);
            sock.close();
            return;
        }

        auto conn = std::make_shared<TcpConnection>(std::move(sock), id, ep);
        {
            std::lock_guard lock(conn_mu_);
            connections_[id] = conn;
        }

        LOG_INFO("[TCP] Registered connection #{} ({}:{})", id, ep.address, ep.port);
        start_read(std::move(conn));
    }

    void start_read(std::shared_ptr<TcpConnection> conn) {
        // Захватываем shared_ptr в лямбду, чтобы объект жил пока идет чтение
        conn->socket.async_read_some(asio::buffer(conn->read_buf),
            [this, conn](auto ec, std::size_t n) {
                if (ec) {
                    // Обработка дисконнекта
                    int err_code = (ec == asio::error::eof || ec == asio::error::operation_aborted) ? 0 : ec.value();
                    
                    LOG_INFO("[TCP] Connection #{} disconnected: {}", conn->id, ec.message());
                    
                    notify_disconnect(conn->id, err_code);
                    
                    std::lock_guard lock(conn_mu_);
                    connections_.erase(conn->id);
                } else {
                    LOG_TRACE("[TCP] Received {} bytes from #{}", n, conn->id);
                    notify_data(conn->id, conn->read_buf, n);
                    start_read(std::move(conn));
                }
            });
    }

    // ─── Состояние ───────────────────────────────────────────────────────────

    asio::io_context io_;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work_;
    std::unique_ptr<tcp::acceptor> acceptor_;
    std::thread io_thread_;

    std::mutex conn_mu_;
    std::unordered_map<conn_id_t, std::shared_ptr<TcpConnection>> connections_;
};

} // namespace gn

// Регистрация плагина
CONNECTOR_PLUGIN(gn::TcpConnector)
