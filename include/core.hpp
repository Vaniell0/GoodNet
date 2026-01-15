#pragma once

#include <memory>
#include <boost/asio.hpp>
#include "../sdk/plugin.h"
#include "config.hpp"
#include "signals.hpp"

namespace gn {

class HomeServices;
class PluginManager;
class ConnectManager;

class Core {
public:
    explicit Core(Config& config);
    ~Core();

    Core(const Core&) = delete;
    Core& operator=(const Core&) = delete;
    Core(Core&&) = delete;
    Core& operator=(Core&&) = delete;

    bool start();
    void stop();
    bool is_running() const { return is_running_; }

    // Получение компонентов
    PluginManager& get_plugin_manager() { return *plugin_manager_; }
    ConnectManager& get_connect_manager() { return *connect_manager_; }
    boost::asio::io_context& get_io_context() { return io_context_; }
    host_api_t* get_host_api() { return host_api_.get(); }
    
    // Получение сигналов
    const PacketSignal& get_packet_signal() { return *packet_signal_; }
    const ConnStateSignal& get_conn_state_signal() { return *conn_state_signal_; }

private:
    void initialize_host_api();
    void initialize_components();
    void initialize_io_threads();
    void cleanup();

    // Реализации C API callback-ов
    void send_impl(const char* uri, uint32_t type, const void* data, size_t size);
    handle_t create_connection_impl(const char* uri);
    void close_connection_impl(handle_t handle);
    void update_connection_state_impl(const char* uri, conn_state_t state);

    // C API функции
    static void c_api_send(const char* uri, uint32_t type, const void* data, size_t size);
    static handle_t c_api_create_connection(const char* uri);
    static void c_api_close_connection(handle_t handle);
    static void c_api_update_connection_state(const char* uri, conn_state_t state);

    // Сигналы
    std::unique_ptr<PacketSignal> packet_signal_;
    std::unique_ptr<ConnStateSignal> conn_state_signal_;
    
    std::unique_ptr<host_api_t> host_api_;
    boost::asio::io_context io_context_;
    std::vector<std::thread> io_threads_;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_guard_;
    
    std::unique_ptr<HomeServices> home_services_;
    std::shared_ptr<PluginManager> plugin_manager_;
    std::unique_ptr<ConnectManager> connect_manager_;
    
    Config& config_;
    bool is_running_ = false;
};

} // namespace gn