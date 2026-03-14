# 11 — C SDK

`sdk/types.h` · `sdk/plugin.h` · `sdk/handler.h` · `sdk/connector.h`

C SDK — единственный интерфейс, который должен знать плагин на любом языке с C FFI.

---

## types.h

### Wire framing

```c
#define GNET_MAGIC      0x474E4554U  // ASCII 'GNET' — маркер фрейма
#define GNET_PROTO_VER  2U           // версия header_t

typedef uint64_t conn_id_t;
#define CONN_ID_INVALID 0ULL
```

### Типы сообщений

```c
// Диапазоны: 0–99 core | 100–999 built-in | 1000–9999 user | 10000+ experimental
#define MSG_TYPE_SYSTEM       0u
#define MSG_TYPE_AUTH         1u
#define MSG_TYPE_KEY_EXCHANGE 2u
#define MSG_TYPE_HEARTBEAT    3u
#define MSG_TYPE_ICE_SIGNAL  11u   // ICE/DTLS SDP exchange
#define MSG_TYPE_CHAT       100u
#define MSG_TYPE_FILE       200u
```

### conn_state_t

```c
typedef enum {
    STATE_CONNECTING,
    STATE_AUTH_PENDING,
    STATE_KEY_EXCHANGE,
    STATE_ESTABLISHED,
    STATE_CLOSING,
    STATE_BLOCKED,
    STATE_CLOSED
} conn_state_t;
```

### header_t v2 (44 байта, #pragma pack(push,1))

```
Offset  Size  Поле           Описание
────────────────────────────────────────────────────────────────────
0       4     magic          GNET_MAGIC
4       1     proto_ver      GNET_PROTO_VER (2)
5       1     flags          зарезервировано, 0
6       2     payload_type   MSG_TYPE_* (uint16_t)
8       4     payload_len    байты после заголовка
12      8     packet_id      монотонный счётчик пакетов соединения
20      8     timestamp      время отправки, unix microseconds
28      16    sender_id      первые 16 байт device_pubkey отправителя
```

### endpoint_t

```c
typedef struct {
    char     address[128]; // NUL-terminated IP или hostname
    uint16_t port;
    uint8_t  pubkey[32];   // Ed25519 user pubkey пира (валиден после AUTH)
    uint64_t peer_id;      // conn_id; вставляется ядром при каждом dispatch
} endpoint_t;
```

`peer_id` заполняется ядром перед каждым вызовом `handle_message()`. Используйте `ep->peer_id` как `conn_id` в `api->send_response()` — это прямой ответ без поиска по URI.

---

## propagation_t

```c
typedef enum {
    PROPAGATION_CONTINUE = 0, // передать следующему хендлеру по приоритету
    PROPAGATION_CONSUMED = 1, // остановить цепочку; фиксирует session affinity
    PROPAGATION_REJECT   = 2  // дропнуть пакет молча
} propagation_t;
```

Возвращается из `handler_t::on_message_result()`. Если поле `NULL` — трактуется как `PROPAGATION_CONTINUE`.

---

## plugin_info_t

```c
typedef struct {
    const char* name;      // уникальное имя плагина
    uint32_t    version;   // (major<<16)|(minor<<8)|patch
    uint8_t     priority;  // 255 = первый в цепочке, 0 = последний
    uint8_t     _pad[3];
    uint32_t    caps_mask; // PLUGIN_CAP_* флаги
} plugin_info_t;

#define PLUGIN_CAP_COMPRESS_ZSTD (1U << 0)
#define PLUGIN_CAP_ICE_SUPPORT   (1U << 1)
#define PLUGIN_CAP_HOT_RELOAD    (1U << 2)
```

Экспортируется через `plugin_get_info()`. Должна указывать на **статический** объект внутри `.so` (живёт всё время загрузки плагина).

---

## plugin_state_t

```c
typedef enum {
    PLUGIN_STATE_PREPARING = 0, // загружен; не получает трафик
    PLUGIN_STATE_ACTIVE    = 1, // основной обработчик
    PLUGIN_STATE_DRAINING  = 2, // старая версия; обслуживает существующие сессии
    PLUGIN_STATE_ZOMBIE    = 3  // нет соединений; ожидает dlclose
} plugin_state_t;
```

Готовит ABI для hot-reload (beta). В alpha все плагины сразу `PLUGIN_STATE_ACTIVE`.

---

## plugin_type_t (deprecated)

```c
// Устарел — используйте plugin_info_t::caps_mask вместо него.
// Оставлен для совместимости со старыми .so.
typedef enum {
    PLUGIN_TYPE_UNKNOWN   = 0,
    PLUGIN_TYPE_HANDLER   = 1,
    PLUGIN_TYPE_CONNECTOR = 2
} plugin_type_t;
```

PluginManager по-прежнему пишет это поле в `host_api_t::plugin_type` перед `*_init()`, но читать его в новом коде не нужно — смотрите `api->plugin_info->caps_mask`.

---

## host_api_t (plugin.h)

Единственный канал плагин ↔ ядро. Инъектируется через `*_init()`.

```c
typedef struct host_api_t {
    // ── Connector → Core ──────────────────────────────────────────────────────
    // Регистрирует новое соединение; возвращает conn_id для ассоциации с сокетом.
    conn_id_t (*on_connect)(void* ctx, const endpoint_t* endpoint);

    // Доставляет сырые байты (ядро обрабатывает reassembly).
    void (*on_data)(void* ctx, conn_id_t id, const void* raw, size_t size);

    // Уведомляет ядро о закрытии соединения.
    void (*on_disconnect)(void* ctx, conn_id_t id, int error_code);

    // ── Handler → Core ────────────────────────────────────────────────────────
    // Отправить пакет пиру по URI. Ядро разрешает маршрут и шифрует.
    void (*send)(void* ctx, const char* uri, uint32_t msg_type,
                 const void* payload, size_t size);

    // Ответить напрямую на существующее соединение (без поиска по URI).
    // Используйте endpoint->peer_id (== conn_id) внутри handle_message().
    void (*send_response)(void* ctx, conn_id_t conn_id, uint32_t msg_type,
                          const void* data, size_t len);

    // ── Crypto ────────────────────────────────────────────────────────────────
    // Подписать данные device Ed25519 секретным ключом ядра. 0 = успех.
    int (*sign_with_device)(void* ctx, const void* data, size_t size,
                             uint8_t sig[64]);

    // Верифицировать Ed25519 подпись. 0 = валидна.
    int (*verify_signature)(void* ctx, const void* data, size_t size,
                             const uint8_t* pubkey, const uint8_t* signature);

    // ── Routing helpers ───────────────────────────────────────────────────────
    // Найти conn_id для ESTABLISHED пира по hex-encoded pubkey (64 символа).
    // Возвращает CONN_ID_INVALID если не подключён или не прошёл AUTH.
    conn_id_t (*find_conn_by_pubkey)(void* ctx, const char* pubkey_hex_64);

    // ── Self-registration ─────────────────────────────────────────────────────
    // Зарегистрировать дополнительный handler_t из коннектор-плагина.
    // Вызывать только в on_init() — небезопасно после прихода первого пакета.
    void (*register_handler)(void* ctx, handler_t* h);

    // ── Logging ───────────────────────────────────────────────────────────────
    // Portable лог-шим для плагинов без прямой линковки spdlog.
    // level: 0=trace  1=debug  2=info  3=warn  4=error  5=critical
    void (*log)(void* ctx, int level, const char* file, int line,
                const char* msg);

    // ── Metadata ──────────────────────────────────────────────────────────────
    // Raw spdlog::logger* для sync_plugin_context() (sharing без RTLD_GLOBAL).
    void* internal_logger;

    // Указывает на plugin_info_t, заполненный PluginManager перед *_init().
    // Объект принадлежит PluginManager, не плагину.
    plugin_info_t* plugin_info;

    // @deprecated Оставлен для бинарной совместимости. Используйте plugin_info.
    plugin_type_t plugin_type;

    // Непрозрачный контекст ядра — передавать первым аргументом в все коллбэки.
    void* ctx;
} host_api_t;
```

### Новые поля vs старый ABI

| Поле | Статус | Примечание |
|---|---|---|
| `send_response` | **Новое** | Прямой ответ по conn_id без URI lookup |
| `find_conn_by_pubkey` | **Новое** | Поиск соединения по pubkey пира |
| `register_handler` | **Новое** | Для ICE-коннекторов с back-channel |
| `log` | **Новое** | Portable logging без spdlog |
| `plugin_info` | **Новое** | Указатель на `plugin_info_t` |
| `plugin_type` | **Deprecated** | Замените на `plugin_info->caps_mask` |

---

## handler_t (handler.h)

```c
struct handler_t {
    // Уникальное имя — ключ для PluginManager и SignalBus.
    const char* name;

    // Вызывается для каждого собранного и расшифрованного пакета.
    // endpoint->peer_id == conn_id для ответа через send_response().
    void (*handle_message)(void* user_data, const header_t* header,
                           const endpoint_t* endpoint,
                           const void* payload, size_t payload_size);

    // Хук цепочки ответственности (опционально).
    // Вызывается ядром ПОСЛЕ handle_message().
    // NULL → трактуется как PROPAGATION_CONTINUE.
    propagation_t (*on_message_result)(void* user_data,
                                       const header_t* header,
                                       uint32_t msg_type);

    // Изменение состояния соединения.
    void (*handle_conn_state)(void* user_data, const char* uri,
                               conn_state_t state);

    // Вызывается перед dlclose(). Освободить все ресурсы.
    void (*shutdown)(void* user_data);

    // Типы сообщений для подписки.
    // NULL / num == 0 → wildcard (получает все типы).
    const uint32_t* supported_types;
    size_t          num_supported_types;

    // Самоописание плагина. NULL → PluginManager назначит дефолты.
    const plugin_info_t* info;

    // Непрозрачный контекст плагина.
    void* user_data;
};

typedef int (*handler_init_t)(host_api_t* api, handler_t** out_handler);
typedef const plugin_info_t* (*plugin_get_info_t)(void);
```

### Пример: обработка с ответом

```c
static void my_handle_message(void* ud, const header_t* hdr,
                               const endpoint_t* ep,
                               const void* payload, size_t size) {
    MyCtx* ctx = (MyCtx*)ud;
    // Прямой ответ — не нужен URI:
    ctx->api->send_response(ctx->api->ctx, ep->peer_id,
                             MSG_TYPE_CHAT, payload, size);
}

static propagation_t my_on_result(void* ud, const header_t* hdr,
                                   uint32_t msg_type) {
    return PROPAGATION_CONSUMED; // этот хендлер обработал пакет полностью
}
```

---

## connector_ops_t (connector.h)

```c
typedef struct connector_ops_t {
    // Начать async подключение к uri ("tcp://host:port"). Возврат быстрый.
    int (*connect)(void* connector_ctx, const char* uri);

    // Начать прослушивание. Вызывать api->on_connect() при каждом accept.
    int (*listen)(void* connector_ctx, const char* host, uint16_t port);

    // Записать байты в conn_id.
    int (*send_to)(void* connector_ctx, conn_id_t conn_id,
                   const void* data, size_t size);

    // Закрыть conn_id. Должен в итоге вызвать api->on_disconnect().
    void (*close)(void* connector_ctx, conn_id_t conn_id);

    // Заполнить buf схемой URI: "tcp", "udp", "ws", "ice", "mock".
    void (*get_scheme)(void* connector_ctx, char* buf, size_t buf_size);

    // Заполнить buf человекочитаемым именем.
    void (*get_name)(void* connector_ctx, char* buf, size_t buf_size);

    // Вызывается перед dlclose(). Закрыть все соединения.
    void (*shutdown)(void* connector_ctx);

    void* connector_ctx; // непрозрачный контекст
} connector_ops_t;

typedef int (*connector_init_t)(host_api_t* api, connector_ops_t** out_ops);
typedef const plugin_info_t* (*plugin_get_info_t)(void);
```

---

*← [10 — Config](10-config.md) · [12 — C++ SDK →](12-sdk-cpp.md)*
