#pragma once

// ─── core/connectionManager.hpp ───────────────────────────────────────────────
//
// Протокол рукопожатия (Noise_XX упрощённый):
//
//   Шаг 1 — AUTH (оба шлют одновременно после connect):
//
//     user_pubkey[32]    — Ed25519 публичный ключ пользователя
//     device_pubkey[32]  — Ed25519 публичный ключ устройства
//     signature[64]      — Ed25519(user_sk, user_pk || device_pk || ephem_pk)
//                          ephem_pk включён в подпись → защита от Replay Attack
//     ephem_pubkey[32]   — X25519 эфемерный публичный ключ для ECDH
//     schemes[]          — поддерживаемые транспортные схемы (capability negotiation)
//
//   Шаг 2 — ECDH (локально, без передачи по сети):
//
//     shared = crypto_scalarmult(my_ephem_sk, peer_ephem_pk)
//     session_key = BLAKE2b(shared || min(user_pk_a, user_pk_b) || max(…))
//     → детерминировано с обеих сторон, не зависит от направления соединения
//
//   После ECDH → STATE_ESTABLISHED:
//     - Все payload шифруются: crypto_secretbox(plain, nonce, session_key)
//     - nonce = send_counter (little-endian uint64) padded to 24 bytes
//     - header.signature = Ed25519(device_sk, header_bytes без sig поля)
//     - Эфемерные ключи затираются sodium_memzero
//
//   Localhost (127.x / ::1):
//     AUTH проходит для идентификации пира, но шифрование и подпись
//     пакетов пропускаются (is_localhost = true).
//
// Разбивка по файлам:
//   cm_identity.cpp   — NodeIdentity, SSH key parser
//   cm_session.cpp    — SessionState::encrypt / decrypt / derive
//   cm_handshake.cpp  — handle_connect, send_auth, process_auth, handle_disconnect
//   cm_dispatch.cpp   — handle_data (stream reassembly), dispatch_packet
//   cm_send.cpp       — send, send_frame, negotiate_scheme, register_*

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>
#include <unordered_map>

#include <sodium.h>
#include <fmt/core.h>
#include <fmt/ranges.h>

#include "logger.hpp"
#include "signals.hpp"
#include "data/machine_id.hpp"

#include "../sdk/types.h"
#include "../sdk/handler.h"     // уже включает plugin.h (host_api_t, handler_t)
#include "../sdk/connector.h"   // connector_ops_t

namespace gn {

namespace fs = std::filesystem;

// ─── IdentityConfig ───────────────────────────────────────────────────────────
//
// config.json → "identity": { "dir", "ssh_key", "use_machine_id" }

struct IdentityConfig {
    fs::path dir            = "~/.goodnet";
    fs::path ssh_key_path   = "";   // пусто = автодетект ~/.ssh/id_ed25519
    bool     use_machine_id = true;
};

// ─── NodeIdentity ─────────────────────────────────────────────────────────────

struct NodeIdentity {
    uint8_t user_pubkey  [crypto_sign_PUBLICKEYBYTES];  // 32
    uint8_t user_seckey  [crypto_sign_SECRETKEYBYTES];  // 64
    uint8_t device_pubkey[crypto_sign_PUBLICKEYBYTES];  // 32
    uint8_t device_seckey[crypto_sign_SECRETKEYBYTES];  // 64

    // Основной путь — конфиг из config.json
    static NodeIdentity load_or_generate(const IdentityConfig& cfg);

    // Backward-compat для тестов
    static NodeIdentity load_or_generate(const fs::path& dir) {
        return load_or_generate(IdentityConfig{ .dir = dir });
    }

    std::string user_pubkey_hex()   const;
    std::string device_pubkey_hex() const;

    // Парсер OpenSSH Ed25519 (только незашифрованные ключи)
    static bool try_load_ssh_key(const fs::path& path,
                                  uint8_t out_pub[crypto_sign_PUBLICKEYBYTES],
                                  uint8_t out_sec[crypto_sign_SECRETKEYBYTES]);
private:
    static void load_or_gen_keypair(const fs::path& path,
                                     uint8_t out_pub[crypto_sign_PUBLICKEYBYTES],
                                     uint8_t out_sec[crypto_sign_SECRETKEYBYTES]);
    static void save_key(const fs::path& path, const uint8_t* key, size_t size);
};

// ─── auth_payload_t ───────────────────────────────────────────────────────────
//
// Wire-формат MSG_TYPE_AUTH.
//
// kBaseSize (160) = pubkeys(64) + sig(64) + ephem_pk(32)
// kFullSize (289) = kBaseSize + schemes_count(1) + schemes[8][16]
//
// Backward-compat: старые клиенты шлют payload_len == kBaseSize
//   → schemes_count трактуется как 0.
//
// Подпись: Ed25519(user_sk, user_pk || device_pk || ephem_pk)
// Включение ephem_pk в подпись → перехваченный AUTH бесполезен (Replay Attack).

static constexpr uint8_t AUTH_MAX_SCHEMES = 8;
static constexpr uint8_t AUTH_SCHEME_LEN  = 16;   // включая NUL

#pragma pack(push, 1)
struct auth_payload_t {
    uint8_t user_pubkey  [32];
    uint8_t device_pubkey[32];
    uint8_t signature    [64];  // Ed25519(user_sk, user_pk||device_pk||ephem_pk)
    uint8_t ephem_pubkey [32];  // X25519 — публичная половина для ECDH

    uint8_t schemes_count;
    char    schemes[AUTH_MAX_SCHEMES][AUTH_SCHEME_LEN];

    static constexpr size_t kBaseSize = 32 + 32 + 64 + 32;  // 160
    static constexpr size_t kFullSize = kBaseSize + 1 + AUTH_MAX_SCHEMES * AUTH_SCHEME_LEN; // 289

    void set_schemes(const std::vector<std::string>& sv) {
        schemes_count = static_cast<uint8_t>(
            std::min(sv.size(), static_cast<size_t>(AUTH_MAX_SCHEMES)));
        for (uint8_t i = 0; i < schemes_count; ++i) {
            std::strncpy(schemes[i], sv[i].c_str(), AUTH_SCHEME_LEN - 1);
            schemes[i][AUTH_SCHEME_LEN - 1] = '\0';
        }
        // Инициализируем остаток нулями
        for (uint8_t i = schemes_count; i < AUTH_MAX_SCHEMES; ++i)
            schemes[i][0] = '\0';
    }

    std::vector<std::string> get_schemes() const {
        std::vector<std::string> out; out.reserve(schemes_count);
        for (uint8_t i = 0; i < schemes_count; ++i) out.emplace_back(schemes[i]);
        return out;
    }
};
#pragma pack(pop)

static_assert(sizeof(auth_payload_t) == auth_payload_t::kFullSize,
              "auth_payload_t size mismatch — check padding");

// ─── SessionState ─────────────────────────────────────────────────────────────
//
// Криптографическое состояние соединения после ECDH.
// Хранится в ConnectionRecord.
//
// Деривация session_key:
//   shared = X25519(my_ephem_sk, peer_ephem_pk)            // 32 bytes
//   context = shared || min(user_pk_a, user_pk_b) || max(…) // domain separation
//   session_key = BLAKE2b-256(context)
//
// Шифрование (crypto_secretbox = XSalsa20-Poly1305):
//   nonce = send_nonce_u64 (LE) + 16 нулевых байт → 24 байта
//   wire  = nonce(8) + secretbox(plain, nonce24, session_key)
//
// Автоматическое обнаружение replay: recv_nonce_expected строго возрастает.

struct SessionState {
    uint8_t session_key[crypto_secretbox_KEYBYTES]{};  // 32 bytes, XSalsa20-Poly1305

    bool ready = false;

    // Эфемерные ключи (затираются сразу после derive)
    uint8_t my_ephem_pk[crypto_box_PUBLICKEYBYTES]{};  // 32
    uint8_t my_ephem_sk[crypto_box_SECRETKEYBYTES]{};  // 32

    std::atomic<uint64_t> send_nonce{1};
    std::atomic<uint64_t> recv_nonce_expected{1};

    // Зашифровать plaintext → wire (8-байтный nonce-префикс + ciphertext)
    std::vector<uint8_t> encrypt(const void* plain, size_t plain_len);

    // Расшифровать wire → plaintext.
    // Ожидает nonce-префикс 8 байт + secretbox.
    // Возвращает пустой вектор при ошибке (неверный MAC или replay).
    std::vector<uint8_t> decrypt(const void* wire, size_t wire_len);

    // Затереть эфемерные ключи сразу после derive_session
    void clear_ephemeral() {
        sodium_memzero(my_ephem_sk, sizeof(my_ephem_sk));
        sodium_memzero(my_ephem_pk, sizeof(my_ephem_pk));
    }

    ~SessionState() {
        sodium_memzero(session_key,  sizeof(session_key));
        sodium_memzero(my_ephem_sk, sizeof(my_ephem_sk));
    }

    // Некопируемо (atomic + секретные ключи)
    SessionState()                         = default;
    SessionState(const SessionState&)      = delete;
    SessionState& operator=(const SessionState&) = delete;
    SessionState(SessionState&&)           = delete;
};

// ─── ConnectionRecord ─────────────────────────────────────────────────────────

struct ConnectionRecord {
    conn_id_t    id;
    conn_state_t state        = STATE_AUTH_PENDING;
    endpoint_t   remote;
    std::string  local_scheme;       // схема входящего соединения

    std::vector<std::string> peer_schemes;       // объявлено пиром в AUTH
    std::string  negotiated_scheme;              // выбрано после AUTH (лучшее пересечение)

    uint8_t peer_user_pubkey  [32]{};
    uint8_t peer_device_pubkey[32]{};
    bool    peer_authenticated = false;

    bool    is_localhost = false;                // skip encrypt/sign

    std::unique_ptr<SessionState> session;       // nullptr до ECDH

    std::vector<uint8_t> recv_buf;               // TCP reassembly buffer
};

// ─── HandlerEntry ─────────────────────────────────────────────────────────────

struct HandlerEntry {
    std::string           name;
    handler_t*            handler = nullptr;
    std::vector<uint32_t> subscribed_types;  // пустой = wildcard
};

// ─── ConnectionManager ────────────────────────────────────────────────────────

class ConnectionManager {
public:
    explicit ConnectionManager(SignalBus& bus, NodeIdentity identity);
    ~ConnectionManager();

    ConnectionManager(const ConnectionManager&)            = delete;
    ConnectionManager& operator=(const ConnectionManager&) = delete;

    // ── Регистрация ────────────────────────────────────────────────────────────

    void register_connector(const std::string& scheme, connector_ops_t* ops);
    void register_handler  (handler_t* h);

    // Порядок предпочтений схем для negotiate_scheme()
    void set_scheme_priority(std::vector<std::string> p);

    // ── host_api_t ─────────────────────────────────────────────────────────────
    // Заполнить структуру коллбэков для плагинов

    void fill_host_api(host_api_t* api);

    // ── Отправка ───────────────────────────────────────────────────────────────
    // uri: "tcp://host:port" или "host:port" (автосхема через negotiated_scheme)

    void send(const char* uri, uint32_t msg_type, const void* payload, size_t size);

    // ── Shutdown ───────────────────────────────────────────────────────────────

    void shutdown();

    // ── Инфо ───────────────────────────────────────────────────────────────────

    size_t                      connection_count()        const;
    std::vector<std::string>    get_active_uris()         const;
    std::optional<conn_state_t> get_state(conn_id_t id)  const;
    std::optional<std::string>  get_negotiated_scheme(conn_id_t id) const;
    const NodeIdentity&         identity()                const { return identity_; }

private:
    // ── cm_handshake.cpp ───────────────────────────────────────────────────────
    conn_id_t handle_connect   (const endpoint_t* ep);
    void      handle_disconnect(conn_id_t id, int error);
    void      send_auth        (conn_id_t id);
    bool      process_auth     (conn_id_t id, const uint8_t* payload, size_t size);

    // ── cm_session.cpp ─────────────────────────────────────────────────────────
    bool derive_session(conn_id_t id, const uint8_t peer_ephem_pk[32],
                        const uint8_t peer_user_pk[32]);

    // ── cm_dispatch.cpp ────────────────────────────────────────────────────────
    void handle_data   (conn_id_t id, const void* raw, size_t size);
    void dispatch_packet(conn_id_t id, const header_t* hdr,
                         const uint8_t* payload, size_t payload_size);

    // ── cm_send.cpp ────────────────────────────────────────────────────────────
    // Низкоуровневая отправка готового фрейма через коннектор
    void             send_frame      (conn_id_t id, uint32_t msg_type,
                                      const void* payload, size_t size);
    std::string      negotiate_scheme(const ConnectionRecord& rec) const;
    std::vector<std::string> local_schemes() const;
    std::optional<conn_id_t> resolve_uri(const std::string& uri);
    connector_ops_t* find_connector  (const std::string& scheme);
    static bool      is_localhost_address(std::string_view address);

    // ── C-ABI адаптеры (статические) ──────────────────────────────────────────
    static conn_id_t s_on_connect   (void*, const endpoint_t*);
    static void      s_on_data      (void*, conn_id_t, const void*, size_t);
    static void      s_on_disconnect(void*, conn_id_t, int);
    static void      s_send         (void*, const char*, uint32_t, const void*, size_t);
    static int       s_sign         (void*, const void*, size_t, uint8_t[64]);
    static int       s_verify       (void*, const void*, size_t, const uint8_t*, const uint8_t*);

    // ── State ──────────────────────────────────────────────────────────────────

    SignalBus&    bus_;
    NodeIdentity  identity_;
    std::atomic<bool> shutting_down_{false};

    std::vector<std::string> scheme_priority_ = {"tcp", "ws", "udp", "mock"};

    mutable std::shared_mutex handlers_mu_;
    std::unordered_map<std::string, HandlerEntry> handler_entries_;

    mutable std::shared_mutex records_mu_;
    std::unordered_map<conn_id_t, ConnectionRecord> records_;
    std::atomic<conn_id_t> next_id_{1};

    mutable std::shared_mutex connectors_mu_;
    std::unordered_map<std::string, connector_ops_t*> connectors_;

    mutable std::shared_mutex uri_mu_;
    std::unordered_map<std::string, conn_id_t> uri_index_;

    mutable std::shared_mutex pk_mu_;
    std::unordered_map<std::string, conn_id_t> pk_index_;
};

} // namespace gn
