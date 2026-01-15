#pragma once

#include <boost/asio.hpp>
#include <memory>
#include <unordered_map>
#include <string>
#include <types.h>

namespace gn {

class ConnectManager {
public:
    explicit ConnectManager(boost::asio::io_context& io_context);
    ~ConnectManager();
    
    // Управление соединениями
    handle_t create_connection(const std::string& uri);
    void close_connection(handle_t handle);
    void send_data(handle_t handle, const void* data, size_t size);
    
    // Статистика
    size_t get_connection_count() const { return connections_.size(); }
    bool is_connected(handle_t handle) const;
    
    ConnectManager(const ConnectManager&) = delete;
    ConnectManager& operator=(const ConnectManager&) = delete;
    ConnectManager(ConnectManager&&) = delete;
    ConnectManager& operator=(ConnectManager&&) = delete;

private:
    struct Connection {
        std::string uri;
        void* user_data = nullptr;
        bool is_active = false;
    };
    
    boost::asio::io_context& io_context_;
    std::unordered_map<handle_t, Connection> connections_;
    handle_t next_handle_ = 1;
    
    handle_t generate_handle();
};

} // namespace gn
