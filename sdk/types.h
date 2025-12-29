#pragma once
#include <cstdint>
#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

// Базовые константы
#define GNET_MAGIC 0x474E4554U

// Типы плагинов
typedef enum {
    PLUGIN_TYPE_UNKNOWN = 0,
    PLUGIN_TYPE_HANDLER = 1,
    PLUGIN_TYPE_CONNECTOR = 2
} plugin_type_t;

// Типы сообщений
#define MSG_TYPE_SYSTEM      0
#define MSG_TYPE_AUTH        1
#define MSG_TYPE_KEY_EXCHANGE 2
#define MSG_TYPE_HEARTBEAT   3
#define MSG_TYPE_CHAT        100
#define MSG_TYPE_FILE        200

// Статусы
#define STATUS_OK    0
#define STATUS_ERROR 1

// Состояния соединения
typedef enum {
    STATE_CONNECTING,
    STATE_AUTH_PENDING,
    STATE_KEY_EXCHANGE,
    STATE_ESTABLISHED,
    STATE_CLOSING,
    STATE_BLOCKED,
    STATE_CLOSED
} conn_state_t;

// Структуры (packed для сетевой передачи)
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint64_t packet_id;
    uint64_t timestamp;
    uint32_t payload_type;
    uint16_t status;
    uint16_t reserved;
    uint32_t payload_len;
} header_t;
#pragma pack(pop)

typedef struct {
    char address[128];
    uint16_t port;
    uint64_t peer_id;
} endpoint_t;

typedef uint64_t handle_t;

#ifdef __cplusplus
}
#endif