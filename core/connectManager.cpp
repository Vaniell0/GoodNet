#include "connectManager.hpp"
#include "../include/logger.hpp"
#include "../sdk/types.h"

namespace gn {

ConnectManager::ConnectManager(boost::asio::io_context& io_context)
    : io_context_(io_context) {
    LOG_INFO("ConnectManager initialized");
}

ConnectManager::~ConnectManager() {
    LOG_INFO("ConnectManager shutting down");
    
    // Закрываем все соединения
    for (auto& [handle, conn] : connections_) {
        if (conn.is_active) {
            LOG_DEBUG("Closing connection: handle={}", handle);
        }
    }
    connections_.clear();
}

handle_t ConnectManager::create_connection(const std::string& uri) {
    handle_t handle = generate_handle();
    
    Connection conn {
        .uri = uri,
        .is_active = true
    };
    
    connections_[handle] = std::move(conn);
    
    LOG_INFO("Connection created: handle={}, uri={}", handle, uri);
    return handle;
}

void ConnectManager::close_connection(handle_t handle) {
    auto it = connections_.find(handle);
    if (it != connections_.end()) {
        LOG_INFO("Connection closed: handle={}, uri={}", handle, it->second.uri);
        connections_.erase(it);
    }
}

void ConnectManager::send_data(handle_t handle, const void* data, size_t size) {
    auto it = connections_.find(handle);
    if (it != connections_.end() && it->second.is_active) {
        LOG_DEBUG("Sending data: handle={}, size={} bytes", handle, size);
        // TODO: Реальная отправка данных через соответствующий коннектор
    } else {
        LOG_WARN("Cannot send data: connection {} not found or inactive", handle);
    }
}

bool ConnectManager::is_connected(handle_t handle) const {
    auto it = connections_.find(handle);
    return it != connections_.end() && it->second.is_active;
}

handle_t ConnectManager::generate_handle() {
    return next_handle_++;
}

} // namespace gn
