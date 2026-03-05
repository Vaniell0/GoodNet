#pragma once

// ─── core/data/messages.hpp ───────────────────────────────────────────────────
//
// Типизированные сообщения GoodNet.
// Каждый тип MSG_TYPE_* имеет соответствующий C++ класс.
//
// Правила:
//   • PodData<T>  — фиксированный бинарный layout (AUTH, HEARTBEAT, KEY_EXCHANGE)
//   • JsonData    — расширяемые сообщения (CHAT, FILE metadata)
//   • Все структуры #pragma pack(push,1) — гарантируют wire-совместимость

#include "../../sdk/cpp/data.hpp"
#include "../../sdk/types.h"
#include <cstdint>
#include <string>

namespace gn::msg {

using namespace gn::sdk;

// ─── AUTH ─────────────────────────────────────────────────────────────────────
//
// MSG_TYPE_AUTH = 1
// Отправляется немедленно после установки TCP-соединения (в обе стороны).
// signature = Ed25519(user_seckey, user_pubkey || device_pubkey)

#pragma pack(push, 1)
struct AuthPayload {
    uint8_t user_pubkey  [32];
    uint8_t device_pubkey[32];
    uint8_t signature    [64];
};
#pragma pack(pop)
static_assert(sizeof(AuthPayload) == 128, "AuthPayload size mismatch");

using AuthMessage = PodData<AuthPayload>;

// ─── HEARTBEAT ────────────────────────────────────────────────────────────────
//
// MSG_TYPE_HEARTBEAT = 3
// Пингует пира каждые N секунд. Если нет ответа > timeout → закрыть соединение.

#pragma pack(push, 1)
struct HeartbeatPayload {
    uint64_t timestamp_us;  // unix microseconds
    uint32_t seq;           // монотонный счётчик
    uint8_t  flags;         // 0x01 = reply (pong), 0x00 = ping
    uint8_t  _pad[3];
};
#pragma pack(pop)
static_assert(sizeof(HeartbeatPayload) == 16, "HeartbeatPayload size mismatch");

using HeartbeatMessage = PodData<HeartbeatPayload>;

// ─── KEY_EXCHANGE ─────────────────────────────────────────────────────────────
//
// MSG_TYPE_KEY_EXCHANGE = 2
// X25519 Diffie-Hellman для согласования сессионного ключа.
// После AUTH оба узла генерируют ephemeral X25519 keypair и обмениваются pubkey.
// shared_secret = X25519(my_seckey, peer_pubkey) → derive session_key через HKDF.
//
// Примечание: не используется для localhost-соединений.

#pragma pack(push, 1)
struct KeyExchangePayload {
    uint8_t x25519_pubkey[32];  // ephemeral X25519 pubkey
    uint8_t signature    [64];  // Ed25519(device_seckey, x25519_pubkey) — антиreplay
};
#pragma pack(pop)
static_assert(sizeof(KeyExchangePayload) == 96, "KeyExchangePayload size mismatch");

using KeyExchangeMessage = PodData<KeyExchangePayload>;

// ─── CHAT ─────────────────────────────────────────────────────────────────────
//
// MSG_TYPE_CHAT = 100
// Текстовое сообщение. JSON для расширяемости (nick, timestamp и т.д.)

class ChatMessage final : public JsonData {
public:
    ChatMessage() = default;

    explicit ChatMessage(std::string_view text) {
        data["text"] = std::string(text);
    }

    std::string text()      const { return get<std::string>("text", ""); }
    std::string sender()    const { return get<std::string>("sender", ""); }
    uint64_t    timestamp() const { return get<uint64_t>("timestamp", 0); }

    void set_text    (std::string_view v) { set("text",      std::string(v)); }
    void set_sender  (std::string_view v) { set("sender",    std::string(v)); }
    void set_timestamp(uint64_t ts)       { set("timestamp", ts); }
};

// ─── FILE metadata ────────────────────────────────────────────────────────────
//
// MSG_TYPE_FILE = 200
// Метаданные файла (JSON). Тело файла идёт отдельными чанками (raw bytes).

class FileMetaMessage final : public JsonData {
public:
    FileMetaMessage() = default;

    std::string filename()    const { return get<std::string>("filename", ""); }
    uint64_t    size()        const { return get<uint64_t>("size", 0); }
    std::string sha256()      const { return get<std::string>("sha256", ""); }
    uint32_t    chunk_count() const { return get<uint32_t>("chunk_count", 0); }

    void set_filename   (std::string_view v) { set("filename",    std::string(v)); }
    void set_size       (uint64_t s)         { set("size",        s); }
    void set_sha256     (std::string_view v) { set("sha256",      std::string(v)); }
    void set_chunk_count(uint32_t n)         { set("chunk_count", n); }
};

} // namespace gn::msg
