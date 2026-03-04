#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <sodium.h>

#include "logger.hpp"
#include "signals.hpp"

#include "../sdk/types.h"
#include "../sdk/plugin.h"
#include "../sdk/connector.h"

namespace gn {

// ─── NodeIdentity ─────────────────────────────────────────────────────────────
//
// SSH-style идентификация: два Ed25519 keypair.
//
//   user_key   — аккаунт пользователя
//                Переносится между устройствами (как ~/.ssh/id_ed25519).
//                user_pubkey = публичное "лицо" в сети GoodNet.
//
//   device_key — ключ конкретной машины
//                Генерируется один раз на устройстве.
//                Используется для подтверждения доверия к устройству
//                при регистрации: user подписывает device_pubkey своим ключом.
//
// Хранение:
//   ~/.goodnet/user_key    — 64 байта Ed25519 secret key (chmod 600)
//   ~/.goodnet/device_key  — 64 байта Ed25519 secret key (chmod 600)
//
// При запуске: load_or_generate() читает файлы или создаёт новые.

struct NodeIdentity {
    uint8_t user_pubkey  [crypto_sign_PUBLICKEYBYTES];  // 32 bytes
    uint8_t user_seckey  [crypto_sign_SECRETKEYBYTES];  // 64 bytes — только ядро
    uint8_t device_pubkey[crypto_sign_PUBLICKEYBYTES];  // 32 bytes
    uint8_t device_seckey[crypto_sign_SECRETKEYBYTES];  // 64 bytes — только ядро

    static NodeIdentity load_or_generate(const std::filesystem::path& dir);

    // Hex публичного ключа пользователя — глобальный идентификатор в сети
    std::string user_pubkey_hex()   const;
    std::string device_pubkey_hex() const;
};

// ─── AUTH wire format ─────────────────────────────────────────────────────────
//
// Первый пакет после TCP connect, MSG_TYPE_AUTH:
//
//   [ header_t (magic=GNET_MAGIC, proto_ver, payload_type=AUTH) ]
//   [ auth_payload_t ]
//
// Получатель:
//   1. Проверяет magic и proto_ver → несовпадение = отказ
//   2. Проверяет signature: Ed25519(user_seckey, user_pubkey || device_pubkey)
//      = подтверждение что user действительно владеет device
//   3. Сохраняет peer_user_pubkey и peer_device_pubkey
//   4. Переходит к KEY_EXCHANGE или сразу в ESTABLISHED

#pragma pack(push, 1)
typedef struct {
    uint8_t user_pubkey  [32];  // Ed25519 pubkey аккаунта
    uint8_t device_pubkey[32];  // Ed25519 pubkey устройства
    uint8_t signature    [64];  // Ed25519(user_seckey, user_pubkey || device_pubkey)
} auth_payload_t;
#pragma pack(pop)

// ─── ConnectionRecord ─────────────────────────────────────────────────────────
//
// Лёгкая запись о соединении, хранимая в ядре.
// Плагин хранит тяжёлые объекты (сокеты и т.д.).
// Ядро работает только с conn_id и этой записью.

struct ConnectionRecord {
    conn_id_t    id;
    conn_state_t state  = STATE_AUTH_PENDING;
    endpoint_t   remote;
    std::string  scheme;       // какой коннектор обслуживает

    // Публичные ключи пира (заполняются после AUTH)
    uint8_t peer_user_pubkey  [32]{};
    uint8_t peer_device_pubkey[32]{};
    bool    peer_authenticated = false;

    // Буфер для сборки TCP-фрагментов (stream reassembly)
    std::vector<uint8_t> recv_buf;
};

// ─── ConnectionManager ────────────────────────────────────────────────────────

class ConnectionManager {
public:
    explicit ConnectionManager(PacketSignal& signal, NodeIdentity identity);
    ~ConnectionManager() = default;

    ConnectionManager(const ConnectionManager&)            = delete;
    ConnectionManager& operator=(const ConnectionManager&) = delete;

    // ── Регистрация коннекторов ───────────────────────────────────────────────

    // Вызывать после load_plugin() для каждого коннектора.
    void register_connector(const std::string& scheme, connector_ops_t* ops);

    // ── host_api_t ────────────────────────────────────────────────────────────

    // Заполняет all коллбэки и ctx = this.
    // Передать плагину до вызова connector_init().
    void fill_host_api(host_api_t* api);

    // ── Исходящая отправка (хендлеры → ConnectionManager) ────────────────────

    void send(const char* uri, uint32_t msg_type,
              const void* payload, size_t size);

    // ── Инфо ─────────────────────────────────────────────────────────────────

    size_t connection_count() const;
    std::vector<std::string> get_active_uris() const;
    std::optional<conn_state_t> get_state(conn_id_t id) const;
    const NodeIdentity& identity() const { return identity_; }

private:
    // ── Коллбэки от плагина ───────────────────────────────────────────────────

    conn_id_t handle_connect   (const endpoint_t* ep);
    void      handle_data      (conn_id_t id, const void* raw, size_t size);
    void      handle_disconnect(conn_id_t id, int error);

    // ── AUTH flow ─────────────────────────────────────────────────────────────

    void send_auth       (conn_id_t id);
    bool process_auth    (conn_id_t id,
                          const uint8_t* payload, size_t size);
                          
    // Обработать один собранный пакет.
    void dispatch_packet(conn_id_t id, const header_t* hdr, const uint8_t* payload, size_t payload_size);

    // ── Маршрутизация исходящих ───────────────────────────────────────────────

    std::optional<conn_id_t> resolve_uri(const std::string& uri);
    connector_ops_t*         find_connector(const std::string& scheme);

    // ── Статические C-ABI адаптеры ────────────────────────────────────────────

    static conn_id_t s_on_connect   (void* ctx, const endpoint_t* ep);
    static void      s_on_data      (void* ctx, conn_id_t id,
                                     const void* raw, size_t size);
    static void      s_on_disconnect(void* ctx, conn_id_t id, int err);
    static void      s_send         (void* ctx, const char* uri,
                                     uint32_t type,
                                     const void* payload, size_t size);
    static int       s_sign         (void* ctx,
                                     const void* data, size_t size,
                                     uint8_t sig[64]);
    static int       s_verify       (void* ctx,
                                     const void* data, size_t dsz,
                                     const uint8_t* pk,
                                     const uint8_t* sig);

    // ── Данные ────────────────────────────────────────────────────────────────

    PacketSignal& signal_;
    NodeIdentity  identity_;

    // Реестр соединений (conn_id → record)
    mutable std::shared_mutex         records_mu_;
    std::unordered_map<conn_id_t, ConnectionRecord> records_;
    std::atomic<conn_id_t>            next_id_{1};

    // Реестр коннекторов (scheme → ops*)
    mutable std::shared_mutex         connectors_mu_;
    std::unordered_map<std::string, connector_ops_t*> connectors_;

    // Индексы для быстрого поиска соединений
    mutable std::shared_mutex         uri_mu_;
    std::unordered_map<std::string, conn_id_t> uri_index_;  // "host:port" → id

    mutable std::shared_mutex         pk_mu_;
    std::unordered_map<std::string, conn_id_t> pk_index_;   // pubkey_hex → id
};

} // namespace gn
