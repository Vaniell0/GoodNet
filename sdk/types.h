#pragma once
#include <cstdint>
#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

// ─── Экспорт символов ────────────────────────────────────────────────────────
// GN_EXPORT на handler_init / connector_init гарантирует видимость через dlsym
// даже при сборке с -fvisibility=hidden.

#if defined(_WIN32)
    #define GN_EXPORT __declspec(dllexport)
#else
    #define GN_EXPORT __attribute__((visibility("default")))
#endif

// ─── Версия протокола ─────────────────────────────────────────────────────────

#define GNET_MAGIC     0x474E4554U   // 'GNET'
#define GNET_PROTO_VER 1U

// ─── conn_id_t ────────────────────────────────────────────────────────────────

typedef uint64_t conn_id_t;
#define CONN_ID_INVALID 0ULL

// ─── Типы плагинов ────────────────────────────────────────────────────────────

typedef enum {
    PLUGIN_TYPE_UNKNOWN   = 0,
    PLUGIN_TYPE_HANDLER   = 1,
    PLUGIN_TYPE_CONNECTOR = 2
} plugin_type_t;

// ─── Типы сообщений ───────────────────────────────────────────────────────────

#define MSG_TYPE_SYSTEM       0u
#define MSG_TYPE_AUTH         1u
#define MSG_TYPE_KEY_EXCHANGE 2u
#define MSG_TYPE_HEARTBEAT    3u
#define MSG_TYPE_CHAT       100u
#define MSG_TYPE_FILE       200u

// ─── Состояния соединения ─────────────────────────────────────────────────────

typedef enum {
    STATE_CONNECTING,
    STATE_AUTH_PENDING,
    STATE_KEY_EXCHANGE,
    STATE_ESTABLISHED,
    STATE_CLOSING,
    STATE_BLOCKED,
    STATE_CLOSED
} conn_state_t;

// ─── header_t ─────────────────────────────────────────────────────────────────

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;          // GNET_MAGIC
    uint8_t  proto_ver;      // GNET_PROTO_VER
    uint8_t  flags;          // зарезервировано (0)
    uint16_t reserved;       // зарезервировано (0)
    uint64_t packet_id;      // монотонный счётчик
    uint64_t timestamp;      // unix microseconds
    uint32_t payload_type;   // MSG_TYPE_*
    uint16_t status;         // STATUS_OK / STATUS_ERROR
    uint32_t payload_len;    // размер payload после header
    uint8_t  signature[64];  // Ed25519 подпись; нули до ESTABLISHED
} header_t;
#pragma pack(pop)

// ─── endpoint_t ───────────────────────────────────────────────────────────────

typedef struct {
    char     address[128];   // IP или hostname, NUL-terminated
    uint16_t port;
    uint8_t  pubkey[32];     // Ed25519 user pubkey пира (после AUTH)
    uint64_t peer_id;        // зарезервировано
} endpoint_t;

// ─── Статусы ─────────────────────────────────────────────────────────────────

#define STATUS_OK    0
#define STATUS_ERROR 1

#ifdef __cplusplus
}
#endif
