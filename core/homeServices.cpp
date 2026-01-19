#include "homeServices.hpp"
#include "pluginManager.hpp"
#include "../include/logger.hpp"
#include "signals.hpp"

#include <sodium.h>

namespace gn {

HomeServices::HomeServices(boost::asio::io_context& io_context, std::shared_ptr<PluginManager> plugin_manager)
    : io_context_(io_context), plugin_manager_(plugin_manager) {
    LOG_INFO("HomeServices initialized");
    
    // Инициализируем libsodium для шифрования
    if (sodium_init() < 0) {
        LOG_WARN("Failed to initialize libsodium");
    } else {
        LOG_DEBUG("libsodium initialized");
    }
}

HomeServices::~HomeServices() {
    stop();
    LOG_INFO("HomeServices destroyed");
}

void HomeServices::start(const std::string& listen_address, uint16_t port) {
    if (is_running_) {
        LOG_WARN("HomeServices already running");
        return;
    }
    
    listen_address_ = listen_address;
    listen_port_ = port;
    
    LOG_INFO("Starting HomeServices on {}:{}", listen_address_, listen_port_);
    
    try {
        // Запускаем TCP сервер
        start_tcp_server();
        
        // Запускаем TCP клиент для исходящих соединений
        start_tcp_client();
        
        is_running_ = true;
        LOG_INFO("HomeServices started successfully");
        
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to start HomeServices: {}", e.what());
        throw;
    }
}

void HomeServices::stop() {
    if (!is_running_) {
        return;
    }
    
    LOG_INFO("Stopping HomeServices...");
    
    // Закрываем acceptor
    if (acceptor_) {
        boost::system::error_code ec;
        acceptor_->close(ec);
        if (ec) {
            LOG_WARN("Error closing acceptor: {}", ec.message());
        }
        acceptor_.reset();
    }
    
    is_running_ = false;
    LOG_INFO("HomeServices stopped");
}

void HomeServices::start_tcp_server() {
    try {
        LOG_INFO("Starting TCP server on {}:{}", listen_address_, listen_port_);
        
        boost::asio::ip::tcp::endpoint endpoint(
            boost::asio::ip::make_address(listen_address_),
            listen_port_
        );
        
        acceptor_ = std::make_unique<boost::asio::ip::tcp::acceptor>(io_context_, endpoint);
        
        // Начинаем принимать соединения
        start_accept();
        
        LOG_INFO("TCP server started");
        
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to start TCP server: {}", e.what());
        throw;
    }
}

void HomeServices::start_accept() {
    acceptor_->async_accept(
        [this](boost::system::error_code ec, boost::asio::ip::tcp::socket socket) {
            if (!ec) {
                // Новое соединение установлено
                std::string remote_endpoint = socket.remote_endpoint().address().to_string() + ":" +
                                           std::to_string(socket.remote_endpoint().port());
                LOG_INFO("New connection from: {}", remote_endpoint);
                
                // Создаем обработчик для соединения
                handle_connection(std::move(socket));
                
                // Продолжаем принимать новые соединения
                start_accept();
            } else {
                // Игнорируем ошибку operation_aborted - это нормальное закрытие сервера
                if (ec != boost::asio::error::operation_aborted) {
                    LOG_ERROR("Accept error: {}", ec.message());
                } else {
                    LOG_DEBUG("Accept operation canceled (normal shutdown)");
                }
            }
        }
    );
}

void HomeServices::handle_connection(boost::asio::ip::tcp::socket socket) {
    // Создаем shared_ptr для управления временем жизни соединения
    auto conn = std::make_shared<Connection>(std::move(socket));
    
    // Начинаем чтение данных
    conn->start();
}

void HomeServices::start_tcp_client() {
    // Инициализация TCP клиента для исходящих соединений
    LOG_INFO("TCP client initialized");
    
    // TODO: Реализовать пул исходящих соединений
}

// Реализация класса Connection
HomeServices::Connection::Connection(boost::asio::ip::tcp::socket socket)
    : socket_(std::move(socket)), buffer_(4096) {
    
    // Генерируем уникальный ID для соединения
    connection_id_ = std::to_string(reinterpret_cast<uintptr_t>(this));
    
    LOG_DEBUG("Connection created: {}", connection_id_);
}

HomeServices::Connection::~Connection() {
    LOG_DEBUG("Connection destroyed: {}", connection_id_);
}

void HomeServices::Connection::start() {
    // Устанавливаем тайм-аут на чтение
    boost::asio::steady_timer timer(socket_.get_executor());
    timer.expires_after(std::chrono::seconds(30));
    
    // Начинаем асинхронное чтение
    async_read_header();
}

void HomeServices::Connection::async_read_header() {
    auto self = shared_from_this();
    
    // Читаем заголовок (фиксированный размер)
    boost::asio::async_read(socket_,
        boost::asio::buffer(&current_header_, sizeof(header_t)),
        [this, self](boost::system::error_code ec, std::size_t length) {
            if (!ec && length == sizeof(header_t)) {
                LOG_DEBUG("Received header: packet_id={}, type={}, len={}",
                         current_header_.packet_id,
                         current_header_.payload_type,
                         current_header_.payload_len);
                
                // Проверяем магическое число
                if (current_header_.magic != GNET_MAGIC) {
                    LOG_WARN("Invalid magic: 0x{:08X}", current_header_.magic);
                    close();
                    return;
                }
                
                // Читаем данные
                async_read_data();
            } else {
                if (ec) {
                    LOG_WARN("Read header error: {}", ec.message());
                } else {
                    LOG_WARN("Incomplete header: {} bytes", length);
                }
                close();
            }
        }
    );
}

void HomeServices::Connection::async_read_data() {
    auto self = shared_from_this();
    
    // Подготавливаем буфер для данных
    if (current_header_.payload_len > 0 && current_header_.payload_len <= 1024 * 1024) { // Ограничение 1MB
        buffer_.resize(current_header_.payload_len);
        
        boost::asio::async_read(socket_,
            boost::asio::buffer(buffer_),
            [this, self](boost::system::error_code ec, std::size_t length) {
                if (!ec && length == current_header_.payload_len) {
                    // Обрабатываем полученное сообщение
                    process_message();
                    
                    // Читаем следующее сообщение
                    async_read_header();
                } else {
                    if (ec) {
                        LOG_WARN("Read data error: {}", ec.message());
                    } else {
                        LOG_WARN("Incomplete data: {} of {} bytes", 
                                length, current_header_.payload_len);
                    }
                    close();
                }
            }
        );
    } else {
        LOG_WARN("Invalid payload length: {}", current_header_.payload_len);
        close();
    }
}

void HomeServices::Connection::close() {
    if (socket_.is_open()) {
        boost::system::error_code ec;
        socket_.close(ec);
        if (ec) {
            LOG_DEBUG("Socket close error: {}", ec.message());
        }
    }
}

void HomeServices::Connection::send_response(const std::string& message) {
    auto self = shared_from_this();
    
    // Создаем простой ответ
    std::string response = "OK: " + message;
    
    boost::asio::async_write(socket_,
        boost::asio::buffer(response.data(), response.size()),
        [this, self, response](boost::system::error_code ec, std::size_t /*length*/) {
            if (!ec) {
                LOG_DEBUG("Response sent: {}", response);
            } else {
                LOG_WARN("Failed to send response: {}", ec.message());
            }
        }
    );
}

void HomeServices::Connection::process_message() {
    // Создаем endpoint информацию
    endpoint_t endpoint;
    auto remote = socket_.remote_endpoint();
    snprintf(endpoint.address, sizeof(endpoint.address), "%s",
             remote.address().to_string().c_str());
    endpoint.port = remote.port();
    
    LOG_INFO("Message from {}:{} - type={}, size={}",
             endpoint.address, endpoint.port,
             current_header_.payload_type, buffer_.size());
    
    if (packet_signal) {
        std::span<const char> payload(buffer_.data(), buffer_.size());
        packet_signal->emit(&current_header_, &endpoint, payload);
    }
    
    // Отправляем ответ (echo)
    send_response("Message received");
}

} // namespace gn
