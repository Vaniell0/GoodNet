#pragma once
#include "connector.h"

#include <string>
#include <memory>
#include <functional>
#include <cstring>
#include <stdexcept>
#include <algorithm>

namespace gn {

class IConnection {
protected:
    connection_ops_t ops_;
    connection_callbacks_t callbacks_;
    
public:
    IConnection() {
        callbacks_.on_data = nullptr;
        callbacks_.on_close = nullptr;
        callbacks_.on_error = nullptr;
        callbacks_.user_data = nullptr;
        
        ops_.send = nullptr;
        ops_.close = nullptr;
        ops_.is_active = nullptr;
        ops_.get_endpoint = nullptr;
        ops_.get_uri = nullptr;
        ops_.set_callbacks = nullptr;
        ops_.conn_ctx = this;
    }
    
    virtual ~IConnection() = default;

    virtual bool do_send(const void* data, size_t size) = 0;
    virtual void do_close() = 0;
    virtual bool is_connected() const = 0;
    virtual endpoint_t get_remote_endpoint() const = 0;
    virtual std::string get_uri_string() const = 0;
    
    virtual void shutdown() {}  // <-- Добавляем shutdown для соединения
    
    void notify_data(const void* data, size_t size) {
        if (callbacks_.on_data) {
            callbacks_.on_data(callbacks_.user_data, data, size);
        }
    }
    
    void notify_close() {
        if (callbacks_.on_close) {
            callbacks_.on_close(callbacks_.user_data);
        }
    }
    
    void notify_error(int error_code) {
        if (callbacks_.on_error) {
            callbacks_.on_error(callbacks_.user_data, error_code);
        }
    }
    
    connection_ops_t* to_c_ops() {
        ops_.send = [](void* ctx, const void* data, size_t size) -> int {
            IConnection* self = static_cast<IConnection*>(ctx);
            return self->do_send(data, size) ? 0 : -1;
        };
        
        ops_.close = [](void* ctx) -> int {
            IConnection* self = static_cast<IConnection*>(ctx);
            self->do_close();
            return 0;
        };
        
        ops_.is_active = [](void* ctx) -> int {
            IConnection* self = static_cast<IConnection*>(ctx);
            return self->is_connected() ? 1 : 0;
        };
        
        ops_.get_endpoint = [](void* ctx, endpoint_t* endpoint) {
            IConnection* self = static_cast<IConnection*>(ctx);
            *endpoint = self->get_remote_endpoint();
        };
        
        ops_.get_uri = [](void* ctx, char* buffer, size_t size) {
            IConnection* self = static_cast<IConnection*>(ctx);
            std::string uri = self->get_uri_string();
            
            if (size > 0) {
                const size_t copy_len = std::min(uri.size(), size - 1);
                std::copy_n(uri.c_str(), copy_len, buffer);
                buffer[copy_len] = '\0';
            } else if (size > 0) {
                buffer[0] = '\0';
            }
        };
        
        ops_.set_callbacks = [](void* ctx, const connection_callbacks_t* cb) {
            IConnection* self = static_cast<IConnection*>(ctx);
            if (cb) { self->callbacks_ = *cb; }
        };
        
        return &ops_;
    }
};

class IConnector {
protected:
    connector_ops_t ops_;
    host_api_t* api_ = nullptr;
    
public:
    IConnector() {
        ops_.connect = nullptr;
        ops_.listen = nullptr;
        ops_.get_scheme = nullptr;
        ops_.get_name = nullptr;
        ops_.shutdown = nullptr;
        ops_.connector_ctx = this;
    }
    
    virtual ~IConnector() = default;
    
    void init(host_api_t* api) {
        if (api && api->api_version != GNET_API_VERSION) {
            throw std::runtime_error("API version mismatch");
        }
        api_ = api;
        on_init();
    }
    
    connector_ops_t* to_c_ops() {
        ops_.connect = [](void* ctx, const char* uri) -> connection_ops_t* {
            IConnector* self = static_cast<IConnector*>(ctx);
            
            std::unique_ptr<IConnection> connection = self->create_connection(uri);
            
            if (connection) {
                IConnection* raw_ptr = connection.release();
                return raw_ptr->to_c_ops();
            }
            
            return nullptr;
        };
        
        ops_.listen = [](void* ctx, const char* host, uint16_t port) -> int {
            IConnector* self = static_cast<IConnector*>(ctx);
            return self->start_listening(host, port) ? 0 : -1;
        };
        
        ops_.get_scheme = [](void* ctx, char* scheme, size_t size) {
            IConnector* self = static_cast<IConnector*>(ctx);
            std::string s = self->get_scheme();
            strncpy(scheme, s.c_str(), size - 1);
            scheme[size - 1] = '\0';
        };
        
        ops_.get_name = [](void* ctx, char* name, size_t size) {
            IConnector* self = static_cast<IConnector*>(ctx);
            std::string n = self->get_name();
            strncpy(name, n.c_str(), size - 1);
            name[size - 1] = '\0';
        };
        
        ops_.shutdown = [](void* ctx) {
            IConnector* self = static_cast<IConnector*>(ctx);
            self->on_shutdown();
        };
        
        return &ops_;
    }
    
    virtual void on_init() {}
    virtual void on_shutdown() {}
    
    virtual std::unique_ptr<IConnection> create_connection(const std::string& uri) = 0;
    
    virtual bool start_listening(const std::string& host, uint16_t port) = 0;
    
    virtual std::string get_scheme() const = 0;
    
    virtual std::string get_name() const = 0;
    
protected:    
    void send(const char* uri, uint32_t type, const void* data, size_t size) {
        if (api_ && api_->send) {
            api_->send(uri, type, data, size);
        }
    }
};

}  /* namespace gn */
