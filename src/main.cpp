#include <filesystem>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

#include "connectionManager.hpp"
#include "pluginManager.hpp"
#include "logger.hpp"
#include "config.hpp"
#include "signals.hpp"

namespace fs = std::filesystem;

int main() {
    // ── 1. Конфигурация ───────────────────────────────────────────────────────

    Config conf;
    conf.load_from_file("config.json");

    Logger::log_level  = conf.get_or<std::string>("logging.level", "info");
    Logger::log_file   = conf.get_or<std::string>("logging.file",  "logs/goodnet.log");
    Logger::max_size   = static_cast<size_t>(
                           conf.get_or<int>("logging.max_size",  10 * 1024 * 1024));
    Logger::max_files  = conf.get_or<int>("logging.max_files", 5);

    LOG_INFO("Booting GoodNet...");

    // ── 2. Идентификатор узла (SSH-style Ed25519 keypair) ─────────────────────

    fs::path key_dir = conf.get_or<std::string>("identity.dir", "~/.goodnet");
    if (key_dir.string().starts_with("~/")) {
        const char* home = std::getenv("HOME");
        key_dir = fs::path(home ? home : ".") / key_dir.string().substr(2);
    }

    auto identity = gn::NodeIdentity::load_or_generate(key_dir);

    // ── 3. IO context + PacketSignal ──────────────────────────────────────────

    boost::asio::io_context ioc;
    auto work_guard = boost::asio::make_work_guard(ioc);

    gn::PacketSignal packet_signal(ioc);

    // ── 4. ConnectionManager ──────────────────────────────────────────────────

    gn::ConnectionManager conn_mgr(packet_signal, std::move(identity));

    // ── 5. host_api_t (заполняет ConnectionManager) ──────────────────────────

    static host_api_t api{};
    api.internal_logger = static_cast<void*>(Logger::get().get());
    conn_mgr.fill_host_api(&api);

    // ── 6. Плагины ────────────────────────────────────────────────────────────

    fs::path plugins_path = conf.get_or<std::string>("plugins.base_dir", "");
    if (plugins_path.empty()) {
        const char* env = std::getenv("GOODNET_PLUGINS_DIR");
        plugins_path = env ? env : "./result/plugins";
    }

    gn::PluginManager plugin_mgr(&api, plugins_path);
    plugin_mgr.load_all_plugins();
    plugin_mgr.list_plugins();

    // Регистрируем загруженные коннекторы в ConnectionManager
    // (ConnectionManager шифрует и маршрутизирует через них)
    for (const auto& scheme : {"tcp", "udp", "ws", "mock"}) {
        auto opt = plugin_mgr.find_connector_by_scheme(scheme);
        if (opt) {
            conn_mgr.register_connector(scheme, *opt);
            LOG_INFO("Registered connector '{}' in ConnectionManager", scheme);
        }
    }

    // ── 7. PacketSignal → активные хендлеры ──────────────────────────────────
    //
    // Хендлеры получают уже расшифрованные пакеты от ConnectionManager.
    // Маршрутизация по payload_type.

    packet_signal.connect([&plugin_mgr](std::shared_ptr<header_t> hdr,
                                         const endpoint_t* ep,
                                         gn::PacketData            data) {
        if (hdr->payload_type == MSG_TYPE_CHAT) {
            std::string msg(reinterpret_cast<const char*>(data->data()), data->size());
            std::cout << "\n\033[32m[IN ← " << ep->address << ":" << ep->port << "]\033[0m " 
                      << msg << "\ngoodnet> " << std::flush;
        }

        for (handler_t* h : plugin_mgr.get_active_handlers()) {
            if (!h->handle_message) continue;
            // ... (далее стандартный код вызова хендлеров)
            bool accepted = (h->num_supported_types == 0);
            for (size_t i = 0; !accepted && i < h->num_supported_types; ++i)
                accepted = (h->supported_types[i] == 0 ||
                            h->supported_types[i] == hdr->payload_type);

            if (accepted)
                h->handle_message(h->user_data, hdr.get(), ep, data->data(), data->size());
        }
    });

    // ── 8. IO thread pool ─────────────────────────────────────────────────────

    const int thread_count = std::max(2, static_cast<int>(
                                std::thread::hardware_concurrency()));
    std::vector<std::thread> pool;
    pool.reserve(static_cast<size_t>(thread_count));
    for (int i = 0; i < thread_count; ++i)
        pool.emplace_back([&ioc] { ioc.run(); });

    LOG_INFO("GoodNet ready. {} IO threads.", thread_count);
    LOG_INFO("User pubkey: {}", conn_mgr.identity().user_pubkey_hex());

    // ── 9. CLI ────────────────────────────────────────────────────────────────

    std::cout << R"(
╔══════════════════════════════════════════════════╗
║              GoodNet  —  Node CLI                ║
║                                                  ║
║  listen  <port>         — принять входящие       ║
║  connect <ip> <port>    — подключиться           ║
║  send    <text>         — отправить сообщение    ║
║  whoami                 — показать pubkey        ║
║  status                 — список плагинов        ║
║  exit                   — завершить              ║
╚══════════════════════════════════════════════════╝
)" << std::flush;

    std::string line;
    while (true) {
        std::cout << "goodnet> " << std::flush;
        if (!std::getline(std::cin, line)) break;
        if (line.empty()) continue;

        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;

        // ── exit ──────────────────────────────────────────────────────────────
        if (cmd == "exit" || cmd == "quit") break;

        // ── listen <port> ─────────────────────────────────────────────────────
        else if (cmd == "listen") {
            uint16_t port = 0;
            if (!(iss >> port)) { LOG_ERROR("Usage: listen <port>"); continue; }

            auto opt = plugin_mgr.find_connector_by_scheme("tcp");
            if (!opt) { LOG_ERROR("TCP connector not loaded"); continue; }

            if ((*opt)->listen((*opt)->connector_ctx, "0.0.0.0", port) == 0)
                LOG_INFO("Listening on 0.0.0.0:{}", port);
            else
                LOG_ERROR("listen() failed on port {}", port);
        }

        // ── connect <ip> <port> ───────────────────────────────────────────────
        else if (cmd == "connect") {
            std::string ip; uint16_t port = 0;
            if (!(iss >> ip >> port)) {
                LOG_ERROR("Usage: connect <ip> <port>"); continue;
            }
            std::string uri = fmt::format("tcp://{}:{}", ip, port);

            // ConnectionManager: найдёт TCP коннектор по схеме, установит
            // соединение, получит conn_id через on_connect, запишет в реестр,
            // отправит AUTH-пакет.
            conn_mgr.send(uri.c_str(), MSG_TYPE_SYSTEM, nullptr, 0);
            LOG_INFO("Connection initiated: {}", uri);
        }

        // ── send <text> ───────────────────────────────────────────
        else if (cmd == "send") {
            std::string text;
            if (!(std::getline(iss >> std::ws, text)) || text.empty()) {
                LOG_ERROR("Usage: send <message>"); continue;
            }
            
            auto uris = conn_mgr.get_active_uris();
            if (uris.empty()) {
                LOG_WARN("No active connections."); continue;
            }

            for (const auto& uri : uris) {
                conn_mgr.send(uri.c_str(), MSG_TYPE_CHAT, text.data(), text.size());
            }
            
            std::cout << "\033[34m[SENT TO " << uris.size() << " PEERS]\033[0m " << text << "\n";
        }

        // ── whoami ────────────────────────────────────────────────────────────
        else if (cmd == "whoami") {
            std::cout << "User   pubkey: "
                      << conn_mgr.identity().user_pubkey_hex()   << "\n"
                      << "Device pubkey: "
                      << conn_mgr.identity().device_pubkey_hex() << "\n";
        }

        // ── status ────────────────────────────────────────────────────────────
        else if (cmd == "status") {
            plugin_mgr.list_plugins();
            std::cout << "Active connections: "
                      << conn_mgr.connection_count() << "\n";
        }

        else {
            LOG_ERROR("Unknown command: '{}'. Try: listen / connect / send / whoami / status / exit", cmd);
        }
    }

    // ── 10. Shutdown ──────────────────────────────────────────────────────────

    LOG_INFO("Shutting down...");

    work_guard.reset();
    ioc.stop();
    for (auto& t : pool)
        if (t.joinable()) t.join();

    plugin_mgr.unload_all();
    Logger::shutdown();
    return 0;
}
