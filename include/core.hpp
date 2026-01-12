#pragma once

#include <boost/asio.hpp>

#include "../sdk/plugin.h"

#include "config.hpp"
#include "signals.hpp"

namespace gn {
class Stats;
class PluginManager;
class ConnectManager;
class HomeServices;

class Core {
public:
    Core(Core&&) = delete;
    Core(const Core&) = delete;
    ~Core();

    Core(const Config& config = Config());

    // Управление состоянием
    bool start();
    void stop();
    bool is_running() const;

    static std::unique_ptr<Core>& get() { return instance_; }
    static Core& instance() { return *instance_; }
    
    void init_host_api();
    const std::unique_ptr<host_api_t>& get_host_api() { return host_api_; }

    // Получение компонентов
    const PluginManager& get_plugin_manager() const { return *plugin_manager_; }
    PluginManager& get_plugin_manager() { return *plugin_manager_; }
    
    PacketSignal& get_packet_signal() { return *packet_signal_; }
    ConnStateSignal& get_conn_state_signal() { return *conn_state_signal_; }
    
    boost::asio::io_context& get_io_context() { return io_context_; }

private:
    void initialize_components();
    void initialize_io_threads();
    void cleanup();

    // C API callbacks
    static void c_api_log(const char* msg);
    static void c_api_send(const char* uri, uint32_t type, const void* data, size_t size);
    static handle_t c_api_create_connection(const char* uri);
    static void c_api_close_connection(handle_t handle);
    static void c_api_update_connection_state(const char* uri, conn_state_t state);

    // Реализации callback-ов
    void log_impl(const char* msg);
    void send_impl(const char* uri, uint32_t type, const void* data, size_t size);
    handle_t create_connection_impl(const char* uri);
    void close_connection_impl(handle_t handle);
    void update_connection_state_impl(const char* uri, conn_state_t state);

    // API для плагинов
    std::unique_ptr<host_api_t> host_api_;

    // Обработчики сигналов
    void on_packet_received(const header_t* header,
                           const endpoint_t* endpoint,
                           std::span<const char> payload);
    void on_connection_state_changed(const char* uri, conn_state_t state);
    
    inline static std::unique_ptr<Core> instance_ = nullptr;
    
    // Конфигурация
    Logger& logger_() const;

    // Сетевая конфигурация
    std::string listen_address_;
    uint16_t listen_port_;
    uint io_thread_count_;

    // ASIO
    boost::asio::io_context io_context_;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_guard_;
    std::vector<std::thread> io_threads_;

    // Сигналы
    std::unique_ptr<PacketSignal> packet_signal_;
    std::unique_ptr<ConnStateSignal> conn_state_signal_;
    
    // Менеджеры
    std::unique_ptr<PluginManager> plugin_manager_;
    std::unique_ptr<ConnectManager> connect_manager_;
    std::unique_ptr<HomeServices> home_services_;
};

#define PLUGIN_MANAGER core.get_plugin_manager()
#define PACKET_SIGNAL core.get_packet_signal()
#define HOST_API core.get_host_api()
#define CONFIG core.get_config()

} // namespace gn