#pragma once

#include <memory>
#include <string>
#include <vector>
#include "../sdk/types.h"

namespace gn {

class PluginManager;

class HomeServices {
private:
    // Класс для управления одним соединением
    class Connection : public std::enable_shared_from_this<Connection> {
    public:
        explicit Connection(boost::asio::ip::tcp::socket socket);
        ~Connection();
        
        void start();
        
    private:
        void async_read_header();
        void async_read_data();
        void process_message();
        void send_response(const std::string &message);
        void close();

        boost::asio::ip::tcp::socket socket_;
        std::vector<char> buffer_;
        header_t current_header_;
        std::string connection_id_;
    };
    
public:
    HomeServices(boost::asio::io_context& io_context, std::shared_ptr<PluginManager> plugin_manager);
    ~HomeServices();
    
    void start(const std::string& listen_address, uint16_t port);
    void stop();
    
    HomeServices(const HomeServices&) = delete;
    HomeServices& operator=(const HomeServices&) = delete;
    HomeServices(HomeServices&&) = delete;
    HomeServices& operator=(HomeServices&&) = delete;

private:
    void start_tcp_server();
    void start_tcp_client();
    void start_accept();
    void handle_connection(boost::asio::ip::tcp::socket socket);
    
    boost::asio::io_context& io_context_;
    std::shared_ptr<PluginManager> plugin_manager_;
    
    // TCP сервер для домашних сервисов
    std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor_;
    std::string listen_address_;
    uint16_t listen_port_;
    bool is_running_ = false;
};

} // namespace gn
