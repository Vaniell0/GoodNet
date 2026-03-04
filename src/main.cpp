#include <filesystem>
#include <iostream>
#include <sstream>
#include <thread>
#include <atomic>
#include <cstring>
#include <vector>

#include "pluginManager.hpp"
#include "logger.hpp"
#include "config.hpp"
#include "signals.hpp"

namespace fs = std::filesystem;

// ─── Packet helpers ───────────────────────────────────────────────────────────

/// Build a framed packet: header_t + raw text payload.
static std::vector<char> make_packet(uint32_t type, std::string_view text) {
    header_t hdr{};
    hdr.magic        = GNET_MAGIC;
    hdr.payload_type = type;
    hdr.payload_len  = static_cast<uint32_t>(text.size());

    std::vector<char> pkt(sizeof(hdr) + text.size());
    std::memcpy(pkt.data(), &hdr, sizeof(hdr));
    std::memcpy(pkt.data() + sizeof(hdr), text.data(), text.size());
    return pkt;
}

/// Try to decode a GoodNet-framed message from raw bytes.
/// Returns the text payload if magic matches, empty string otherwise.
static std::string decode_packet(const void* data, size_t size) {
    if (size < sizeof(header_t)) return {};
    const auto* hdr = static_cast<const header_t*>(data);
    if (hdr->magic != GNET_MAGIC) return {};
    if (size < sizeof(header_t) + hdr->payload_len) return {};
    return {static_cast<const char*>(data) + sizeof(header_t), hdr->payload_len};
}

// ─── main ─────────────────────────────────────────────────────────────────────

int main() {
    // ── 1. Logger & Config ────────────────────────────────────────────────────

    Logger::log_level         = "info";
    Logger::source_detail_mode = 3;   // compact: [filename] only

    Config conf{true};
    conf.load_from_file("config.json");

    LOG_INFO("Booting GoodNet demo node...");

    // ── 2. Active connection handle (CLI thread writes, IO thread reads) ──────
    //
    // connection_ops_t* is a non-owning pointer into TcpConnector's storage.
    // Guarded by atomic_flag for the "connection registered" check only;
    // actual concurrent use of send/close goes through Boost.Asio which
    // already serialises via its own strand inside the connector.

    static std::atomic<connection_ops_t*> active_conn{nullptr};

    // ── 3. Host API ───────────────────────────────────────────────────────────
    //
    // api.send is the single data ingress point from connectors to the core.
    // Both server-side incoming data AND outgoing echo from handlers arrive here.

    host_api_t api{};
    api.internal_logger = static_cast<void*>(Logger::get().get());

    api.update_connection_state = [](const char* uri, conn_state_t state) {
        const char* names[] = {
            "CONNECTING","AUTH_PENDING","KEY_EXCHANGE",
            "ESTABLISHED","CLOSING","BLOCKED","CLOSED"
        };
        const char* name = (state <= STATE_CLOSED) ? names[state] : "UNKNOWN";
        LOG_INFO("[NET] {} → {}", uri, name);
    };

    // Server-side incoming data arrives here (wired in tcp.cpp do_accept).
    // Client-side incoming data is handled via per-connection on_data callback
    // set after connect (see below) and does NOT go through api.send.
    api.send = [](const char* /*uri*/, uint32_t /*type*/,
                              const void* data, size_t size) {
        // Guard: tcp.cpp do_accept passes raw bytes from remote peer.
        // If it's a framed GoodNet packet — print it.
        // If not (e.g. plain text from netcat) — print raw.
        std::string msg = decode_packet(data, size);
        if (msg.empty() && size > 0)
            msg = std::string(static_cast<const char*>(data), size);

        // Print with a fresh prompt so the CLI stays tidy.
        std::cout << "\n\033[32m[IN]\033[0m " << msg << "\ngoodnet> " << std::flush;
    };

    // ── 4. Plugin manager ─────────────────────────────────────────────────────

    gn::PluginManager manager(&api, "./result/plugins");
    manager.load_all_plugins();

    // Locate the TCP connector by scheme.
    connector_ops_t* tcp = nullptr;
    {
        char scheme[32];
        for (auto* c : manager.get_active_connectors()) {
            c->get_scheme(c->connector_ctx, scheme, sizeof(scheme));
            if (std::string_view(scheme) == "tcp") { tcp = c; break; }
        }
    }
    if (!tcp) {
        LOG_CRITICAL("TCP connector not loaded — check ./result/plugins/connectors/");
        return 1;
    }

    // ── 5. PacketSignal → handlers ────────────────────────────────────────────
    //
    // Normally the core routes packets from connectors to handlers via this
    // signal. For the demo we wire it up so outgoing sends also pass through
    // the handler chain (e.g. the bundle-logger handler sees every message).

    boost::asio::io_context ioc;
    auto work = boost::asio::make_work_guard(ioc);
    gn::PacketSignal packet_signal(ioc);

    packet_signal.connect(
        [&manager](std::shared_ptr<header_t> hdr, const endpoint_t* ep,
                   gn::PacketData payload) {
            for (auto* h : manager.get_active_handlers()) {
                bool accept = (h->num_supported_types == 0);
                for (size_t i = 0; !accept && i < h->num_supported_types; ++i)
                    accept = (h->supported_types[i] == 0 ||
                              h->supported_types[i] == hdr->payload_type);
                if (accept && h->handle_message)
                    h->handle_message(h->user_data, hdr.get(), ep,
                                      payload->data(), payload->size());
            }
        });

    std::thread io_thread([&ioc] { ioc.run(); });

    // ── 6. CLI ────────────────────────────────────────────────────────────────

    std::cout << R"(
╔══════════════════════════════════════════════════════╗
║              GoodNet  —  Demo Node                   ║
║                                                      ║
║  Two-terminal cross-connect demo:                    ║
║    Terminal A:  listen 11000                         ║
║    Terminal B:  connect 127.0.0.1 11000              ║
║    Terminal B:  send Hello from B!                   ║
║    Terminal A:  send Hello back from A!              ║
║                                                      ║
║  Commands:                                           ║
║    listen  <port>                                    ║
║    connect <ip> <port>                               ║
║    send    <message text>                            ║
║    status                                            ║
║    exit                                              ║
╚══════════════════════════════════════════════════════╝
)";

    // Per-connection callback for CLIENT-side (outgoing) connections.
    // Stored as a raw struct; lifetime matches active_conn.
    // One active connection at a time is enough for the demo.
    connection_callbacks_t client_cbs{};
    client_cbs.user_data = nullptr;
    client_cbs.on_data = [](void*, const void* data, size_t size) {
        std::string msg = decode_packet(data, size);
        if (msg.empty() && size > 0)
            msg = std::string(static_cast<const char*>(data), size);
        std::cout << "\n\033[36m[IN]\033[0m " << msg << "\ngoodnet> " << std::flush;
    };
    client_cbs.on_close = [](void*) {
        std::cout << "\n\033[33m[NET]\033[0m Connection closed\ngoodnet> " << std::flush;
    };
    client_cbs.on_error = [](void*, int code) {
        std::cout << "\n\033[31m[NET]\033[0m Connection error: " << code
                  << "\ngoodnet> " << std::flush;
    };

    std::string line;
    while (true) {
        std::cout << "goodnet> " << std::flush;
        if (!std::getline(std::cin, line)) break;
        if (line.empty()) continue;

        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;

        // ── exit ──────────────────────────────────────────────────────────────
        if (cmd == "exit" || cmd == "quit") {
            break;
        }

        // ── listen <port> ─────────────────────────────────────────────────────
        else if (cmd == "listen") {
            uint16_t port = 0;
            if (!(iss >> port)) { LOG_ERROR("Usage: listen <port>"); continue; }

            int rc = tcp->listen(tcp->connector_ctx, "0.0.0.0", port);
            if (rc == 0)
                LOG_INFO("Listening on 0.0.0.0:{} — waiting for connections", port);
            else
                LOG_ERROR("Failed to listen on port {}", port);
        }

        // ── connect <ip> <port> ───────────────────────────────────────────────
        else if (cmd == "connect") {
            std::string ip; uint16_t port = 0;
            if (!(iss >> ip >> port)) {
                LOG_ERROR("Usage: connect <ip> <port>"); continue;
            }

            std::string uri = "tcp://" + ip + ":" + std::to_string(port);
            LOG_INFO("Connecting to {}...", uri);

            connection_ops_t* conn =
                tcp->connect(tcp->connector_ctx, uri.c_str());

            if (!conn) {
                LOG_ERROR("Failed to connect to {}", uri);
                continue;
            }

            // Register the incoming-data callback.
            conn->set_callbacks(conn->conn_ctx, &client_cbs);
            active_conn.store(conn);
            LOG_INFO("Connected — ready to send. Try: send Hello!");
        }

        // ── send <text> ───────────────────────────────────────────────────────
        else if (cmd == "send") {
            std::string text;
            std::getline(iss >> std::ws, text);
            if (text.empty()) { LOG_ERROR("Usage: send <text>"); continue; }

            auto* conn = active_conn.load();
            if (!conn) { LOG_ERROR("No active connection. Use 'connect' first."); continue; }

            // Build framed packet.
            auto pkt = make_packet(MSG_TYPE_CHAT, text);

            int rc = conn->send(conn->conn_ctx, pkt.data(), pkt.size());
            if (rc == 0) {
                LOG_INFO("\033[34m[OUT]\033[0m {}", text);

                // Also push through the local handler chain so bundle-logger
                // records outgoing messages too.
                auto hdr_copy = std::make_shared<header_t>();
                *hdr_copy = *reinterpret_cast<const header_t*>(pkt.data());
                auto payload = std::make_shared<std::vector<char>>(
                    pkt.begin() + sizeof(header_t), pkt.end());
                static endpoint_t local_ep{};
                std::strncpy(local_ep.address, "127.0.0.1",
                             sizeof(local_ep.address) - 1);
                packet_signal.emit(hdr_copy, &local_ep, payload);
            } else {
                LOG_ERROR("Send failed");
            }
        }

        // ── status ────────────────────────────────────────────────────────────
        else if (cmd == "status") {
            manager.list_plugins();
            auto* conn = active_conn.load();
            std::cout << "Active connection: "
                      << (conn ? "YES" : "none") << "\n";
        }

        else {
            LOG_ERROR("Unknown command: '{}'  (try: listen / connect / send / status / exit)", cmd);
        }
    }

    // ── 7. Shutdown ───────────────────────────────────────────────────────────

    LOG_INFO("Shutting down...");

    work.reset();
    ioc.stop();
    if (io_thread.joinable()) io_thread.join();

    manager.unload_all();
    Logger::shutdown();
    return 0;
}
