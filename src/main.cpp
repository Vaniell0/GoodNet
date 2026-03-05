#include <filesystem>
#include <sstream>
#include <thread>
#include <vector>
#include <csignal>
#include <atomic>
#include <mutex>
#include <unistd.h>
#include <iostream>

#include <fmt/core.h>
#include <fmt/color.h>
#include <fmt/ranges.h>
#include <boost/asio.hpp>

#include "connectionManager.hpp"
#include "pluginManager.hpp"
#include "logger.hpp"
#include "config.hpp"
#include "signals.hpp"

namespace fs = std::filesystem;

// ─── UI ───────────────────────────────────────────────────────────────────────
//
// Весь вывод сериализован через print_mu.
//
// Основной поток:
//   1. ui::prompt() — выводит "goodnet > ", ставит g_waiting = true
//   2. std::getline (блокирует)
//   3. g_waiting = false
//   4. обрабатывает команду, вызывает ui::ok/warn/err
//
// IO thread (SignalBus callback):
//   ui::incoming() — берёт print_mu, стирает строку, печатает сообщение,
//   если g_waiting == true перерисовывает промпт (чтобы пользователь видел куда вводить)
//
// Правило: никто не вызывает fmt::print без захвата print_mu.

namespace ui {

static std::mutex        print_mu;
static std::atomic<bool> g_waiting{false};

static std::string_view type_name(uint32_t t) noexcept {
    switch (t) {
        case MSG_TYPE_SYSTEM:       return "SYS";
        case MSG_TYPE_AUTH:         return "AUTH";
        case MSG_TYPE_KEY_EXCHANGE: return "KX";
        case MSG_TYPE_HEARTBEAT:    return "HB";
        case MSG_TYPE_CHAT:         return "CHAT";
        case MSG_TYPE_FILE:         return "FILE";
        default:                    return "???";
    }
}

// Вызывать только под захваченным print_mu
static void reprompt() {
    fmt::print(fg(fmt::color::steel_blue) | fmt::emphasis::bold, "goodnet");
    fmt::print(fmt::emphasis::faint, " > ");
    std::fflush(stdout);
}

// Показать промпт (основной поток, без lock внутри)
static void prompt() {
    std::lock_guard lk(print_mu);
    reprompt();
}

// Вывод входящего пакета из IO thread.
// Вызывается из SignalBus strand — параллельно с основным потоком.
static void incoming(uint32_t msg_type, const endpoint_t* ep,
                      const void* data, size_t size) {
    std::lock_guard lk(print_mu);

    // ANSI: вернуться в начало строки и стереть её полностью
    // Работает в любом POSIX-терминале (xterm, iTerm2, alacritty, tmux).
    fmt::print("\r\033[2K");

    fmt::print(fg(fmt::color::lime_green) | fmt::emphasis::bold,
               "[{}]", type_name(msg_type));
    fmt::print(fmt::emphasis::faint, " {}:{}  ", ep->address, ep->port);

    if (msg_type == MSG_TYPE_CHAT) {
        // Текстовое сообщение — выводим как есть
        fmt::print("{}", std::string_view(static_cast<const char*>(data), size));
    } else if (size == 0) {
        fmt::print(fmt::emphasis::faint, "<empty>");
    } else {
        // Прочие типы: hex до 32 байт + общий размер
        const size_t show  = std::min(size, size_t{32});
        const auto*  b     = static_cast<const uint8_t*>(data);
        std::string  hex;
        hex.reserve(show * 3);
        for (size_t i = 0; i < show; ++i)
            hex += fmt::format("{:02x} ", b[i]);
        if (size > show) hex += "…";
        fmt::print(fmt::emphasis::faint, "[{} B] {}", size, hex);
    }
    fmt::print("\n");

    // Перерисовать промпт если основной поток ждёт ввода
    if (g_waiting.load(std::memory_order_acquire))
        reprompt();
}

// Вспомогательные функции для основного потока.
// Берут print_mu — безопасно вызывать в любом месте CLI-цикла.

static void ok  (std::string_view s) {
    std::lock_guard lk(print_mu);
    fmt::print(fg(fmt::color::lime_green), "  {}\n", s);
}
static void warn(std::string_view s) {
    std::lock_guard lk(print_mu);
    fmt::print(fg(fmt::color::gold), "  {}\n", s);
}
static void err(std::string_view s) {
    std::lock_guard lk(print_mu);
    fmt::print(fg(fmt::color::orange_red), "  {}\n", s);
}
static void info(std::string_view s) {
    std::lock_guard lk(print_mu);
    fmt::print(fmt::emphasis::faint, "  {}\n", s);
}

static void banner(std::string_view user_hex, std::string_view dev_hex) {
    std::lock_guard lk(print_mu);
    fmt::print("\n");
    fmt::print(fg(fmt::color::steel_blue) | fmt::emphasis::bold,
               "  ╔════════════════════════════════╗\n"
               "  ║  G O O D N E T  v0.1.0-alpha   ║\n"
               "  ╚════════════════════════════════╝\n\n");
    fmt::print(fmt::emphasis::faint,   "  user   ");
    fmt::print(fg(fmt::color::white) | fmt::emphasis::bold, "{}…\n", user_hex.substr(0, 16));
    fmt::print(fmt::emphasis::faint,   "  device ");
    fmt::print(fg(fmt::color::white) | fmt::emphasis::bold, "{}…\n\n", dev_hex.substr(0, 16));
    fmt::print(fmt::emphasis::faint,
               "  listen  connect  send  peers  plugins  whoami  help  exit\n\n");
}

static void help() {
    std::lock_guard lk(print_mu);
    fmt::print("\n");
    fmt::print(fg(fmt::color::steel_blue) | fmt::emphasis::bold, "  Commands\n");
    fmt::print(fmt::emphasis::faint,
               "  ─────────────────────────────────────────\n");
    auto row = [](std::string_view c, std::string_view a, std::string_view d) {
        fmt::print("  ");
        fmt::print(fg(fmt::color::lime_green) | fmt::emphasis::bold, "{:<10}", c);
        fmt::print("{:<22}", a);
        fmt::print(fmt::emphasis::faint, "{}\n", d);
    };
    row("listen",  "<port>",       "начать слушать TCP");
    row("connect", "<ip> <port>",  "подключиться к пиру");
    row("send",    "<текст>",      "отправить CHAT всем пирам");
    row("peers",   "",             "список соединений");
    row("plugins", "",             "список плагинов");
    row("whoami",  "",             "показать ключи узла");
    row("help",    "",             "эта справка");
    row("exit",    "",             "завершить работу");
    fmt::print("\n");
}

} // namespace ui

// ─── Signal handling ──────────────────────────────────────────────────────────

static std::atomic<bool> g_shutdown{false};

static void handle_signal(int) {
    g_shutdown.store(true, std::memory_order_relaxed);
    ::close(STDIN_FILENO);  // разблокирует std::getline (async-signal-safe)
}

// ─── main ─────────────────────────────────────────────────────────────────────

int main() {
    std::signal(SIGINT,  handle_signal);
    std::signal(SIGTERM, handle_signal);
    std::signal(SIGPIPE, SIG_IGN);

    // ── 1. Config ─────────────────────────────────────────────────────────────
    Config conf;
    conf.load_from_file("config.json");

    Logger::log_level = conf.get_or<std::string>("logging.level", "trace");
    Logger::log_file  = conf.get_or<std::string>("logging.file",  "logs/goodnet.log");
    Logger::max_size  = static_cast<size_t>(conf.get_or<int>("logging.max_size", 10 << 20));
    Logger::max_files = conf.get_or<int>("logging.max_files", 5);
    LOG_INFO("Booting GoodNet...");

    // ── 2. Identity ───────────────────────────────────────────────────────────
    auto expand = [](const std::string& s) -> fs::path {
        if (s.starts_with("~/")) {
            const char* h = std::getenv("HOME");
            return fs::path(h ? h : ".") / s.substr(2);
        }
        return s;
    };

    gn::IdentityConfig id_cfg;
    id_cfg.dir            = expand(conf.get_or<std::string>("identity.dir", "~/.goodnet"));
    id_cfg.use_machine_id = conf.get_or<bool>("identity.use_machine_id", true);
    if (const auto sk = conf.get_or<std::string>("identity.ssh_key", ""); !sk.empty())
        id_cfg.ssh_key_path = expand(sk);

    auto identity = gn::NodeIdentity::load_or_generate(id_cfg);

    // ── 3. IO context ─────────────────────────────────────────────────────────
    boost::asio::io_context ioc;
    auto work = boost::asio::make_work_guard(ioc);

    // ── 4. SignalBus ──────────────────────────────────────────────────────────
    //
    // bus[msg_type][handler_name] → HandlerPacketSignal
    // Все расшифрованные пакеты идут через него.
    gn::SignalBus bus(ioc);

    // ── 5. ConnectionManager ──────────────────────────────────────────────────
    gn::ConnectionManager conn_mgr(bus, std::move(identity));

    static host_api_t api{};
    api.internal_logger = static_cast<void*>(Logger::get().get());
    conn_mgr.fill_host_api(&api);

    // ── 6. Plugins ────────────────────────────────────────────────────────────
    fs::path plugins_dir = conf.get_or<std::string>("plugins.base_dir", "");
    if (plugins_dir.empty()) {
        const char* env = std::getenv("GOODNET_PLUGINS_DIR");
        plugins_dir = env ? fs::path(env) : fs::path("./result/plugins");
    }

    gn::PluginManager plugin_mgr(&api, plugins_dir);
    plugin_mgr.load_all_plugins();
    plugin_mgr.list_plugins();

    // Регистрируем коннекторы
    for (const char* s : {"tcp", "udp", "ws", "mock"})
        if (auto opt = plugin_mgr.find_connector_by_scheme(s)) {
            conn_mgr.register_connector(s, *opt);
            LOG_INFO("Registered connector '{}'", s);
        }

    // ── 7. Register handlers ──────────────────────────────────────────────────
    for (handler_t* h : plugin_mgr.get_active_handlers())
        conn_mgr.register_handler(h);

    // ── 8. CLI display — wildcard подписка ────────────────────────────────────
    //
    // ИМЕННО ЗДЕСЬ отображаются все входящие пакеты в терминале.
    //
    // Подписываемся как wildcard-subscriber → получаем КАЖДЫЙ входящий пакет
    // любого msg_type (после расшифровки).
    //
    // ui::incoming() выводит пакет в терминал поверх текущей строки ввода,
    // затем перерисовывает промпт.
    //
    // Параллельно те же пакеты достигают плагин-хендлеров по своим каналам
    // (bus[MSG_TYPE_CHAT]["chat_handler"] и т.д.) — CLI display никак не
    // мешает плагинам.
    bus.subscribe_wildcard("cli_display",
        [](std::string_view /*handler_name*/,
           std::shared_ptr<header_t> hdr,
           const endpoint_t*         ep,
           gn::PacketData            data)
        {
            // Служебные пакеты не показываем — они будут спамить
            if (hdr->payload_type == MSG_TYPE_HEARTBEAT ||
                hdr->payload_type == MSG_TYPE_AUTH)
                return;

            ui::incoming(hdr->payload_type, ep, data->data(), data->size());
        });

    // ── 9. IO threads ─────────────────────────────────────────────────────────
    const int n = std::max(2, static_cast<int>(std::thread::hardware_concurrency()));
    std::vector<std::thread> pool;
    pool.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        pool.emplace_back([&ioc] { ioc.run(); });
    LOG_INFO("GoodNet ready — {} IO threads", n);

    // ── 10. CLI ───────────────────────────────────────────────────────────────
    ui::banner(conn_mgr.identity().user_pubkey_hex(),
               conn_mgr.identity().device_pubkey_hex());

    std::string line;
    while (!g_shutdown.load(std::memory_order_relaxed)) {
        ui::prompt();

        // Сообщаем IO thread что мы ждём ввода →
        // incoming() будет перерисовывать промпт после каждого сообщения.
        ui::g_waiting.store(true, std::memory_order_release);
        const bool got = static_cast<bool>(std::getline(std::cin, line));
        ui::g_waiting.store(false, std::memory_order_release);

        if (!got) break;
        if (g_shutdown.load(std::memory_order_relaxed)) break;

        // Trim leading whitespace
        if (const auto s = line.find_first_not_of(" \t"); s != std::string::npos)
            line = line.substr(s);
        else continue;
        if (line.empty()) continue;

        std::istringstream iss(line);
        std::string cmd; iss >> cmd;

        if (cmd == "exit" || cmd == "quit") {
            break;

        } else if (cmd == "help" || cmd == "?") {
            ui::help();

        } else if (cmd == "listen") {
            uint16_t port = 0;
            if (!(iss >> port)) { ui::err("usage: listen <port>"); continue; }
            auto opt = plugin_mgr.find_connector_by_scheme("tcp");
            if (!opt) { ui::err("TCP connector not loaded"); continue; }
            if ((*opt)->listen((*opt)->connector_ctx, "0.0.0.0", port) == 0)
                ui::ok(fmt::format("listening on 0.0.0.0:{}", port));
            else
                ui::err(fmt::format("listen() failed on port {}", port));

        } else if (cmd == "connect") {
            std::string ip; uint16_t port = 0;
            if (!(iss >> ip >> port)) { ui::err("usage: connect <ip> <port>"); continue; }
            const auto uri = fmt::format("tcp://{}:{}", ip, port);
            conn_mgr.send(uri.c_str(), MSG_TYPE_SYSTEM, nullptr, 0);
            ui::warn(fmt::format("connecting → {}", uri));

        } else if (cmd == "send") {
            std::string text;
            if (!std::getline(iss >> std::ws, text) || text.empty()) {
                ui::err("usage: send <text>"); continue;
            }
            const auto uris = conn_mgr.get_active_uris();
            if (uris.empty()) { ui::warn("нет активных соединений"); continue; }
            for (const auto& u : uris)
                conn_mgr.send(u.c_str(), MSG_TYPE_CHAT, text.data(), text.size());
            {
                std::lock_guard lk(ui::print_mu);
                fmt::print(fg(fmt::color::cornflower_blue),
                           "  [→ {} peer{}]",
                           uris.size(), uris.size() > 1 ? "s" : "");
                fmt::print("  {}\n", text);
            }

        } else if (cmd == "peers") {
            const auto uris = conn_mgr.get_active_uris();
            std::lock_guard lk(ui::print_mu);
            if (uris.empty()) {
                fmt::print(fmt::emphasis::faint, "  нет активных соединений\n");
            } else {
                fmt::print(fg(fmt::color::steel_blue) | fmt::emphasis::bold,
                           "  peers ({})\n", uris.size());
                for (const auto& u : uris) {
                    fmt::print("    ");
                    fmt::print(fg(fmt::color::lime_green), "● ");
                    // Показываем схему если известна
                    // (get_negotiated_scheme требует conn_id, uri_index не публичен)
                    fmt::print("{}\n", u);
                }
            }

        } else if (cmd == "plugins") {
            plugin_mgr.list_plugins();

        } else if (cmd == "whoami") {
            std::lock_guard lk(ui::print_mu);
            fmt::print("\n");
            fmt::print(fmt::emphasis::faint,   "  user   ");
            fmt::print(fg(fmt::color::white) | fmt::emphasis::bold,
                       "{}\n", conn_mgr.identity().user_pubkey_hex());
            fmt::print(fmt::emphasis::faint,   "  device ");
            fmt::print(fg(fmt::color::white) | fmt::emphasis::bold,
                       "{}\n\n", conn_mgr.identity().device_pubkey_hex());

        } else {
            ui::err(fmt::format("неизвестная команда '{}' (help для справки)", cmd));
        }
    }

    // ── 11. Shutdown ──────────────────────────────────────────────────────────
    ui::info("завершение…");
    LOG_INFO("Shutting down...");

    conn_mgr.shutdown();
    work.reset();
    ioc.stop();
    for (auto& t : pool) if (t.joinable()) t.join();
    plugin_mgr.unload_all();
    Logger::shutdown();
    return 0;
}
