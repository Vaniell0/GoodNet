# GoodNet — Справочник API

## C-типы (`sdk/types.h`, `sdk/handler.h`, `sdk/connector.h`, `sdk/plugin.h`)

Эти заголовки определяют ABI-границу между ядром и плагинами. Чистый C, совместим с C и C++.

---

### `header_t`

Заголовок пакета для передачи по сети. `#pragma pack(push, 1)` — без выравнивания, безопасен для сетевой передачи.

```c
typedef struct {
    uint32_t magic;        // Должен быть GNET_MAGIC (0x474E4554 = "GNET")
    uint64_t packet_id;    // Монотонно возрастающий ID
    uint64_t timestamp;    // Unix timestamp (микросекунды)
    uint32_t payload_type; // Тип сообщения уровня приложения (см. MSG_TYPE_*)
    uint16_t status;       // STATUS_OK (0) или STATUS_ERROR (1)
    uint16_t reserved;     // Должен быть ноль
    uint32_t payload_len;  // Длина payload в байтах, следующего за этим заголовком
} header_t;
```

Константы `payload_type`:

| Константа | Значение | Смысл |
|---|---|---|
| `MSG_TYPE_SYSTEM` | 0 | Системный / wildcard (получают все обработчики) |
| `MSG_TYPE_AUTH` | 1 | Аутентификация |
| `MSG_TYPE_KEY_EXCHANGE` | 2 | Криптографический обмен ключами |
| `MSG_TYPE_HEARTBEAT` | 3 | Keep-alive |
| `MSG_TYPE_CHAT` | 100 | Чат-сообщение |
| `MSG_TYPE_FILE` | 200 | Передача файла |

---

### `endpoint_t`

Описывает удалённый узел соединения.

```c
typedef struct {
    char     address[128]; // Null-terminated строка IP-адреса
    uint16_t port;         // Удалённый порт
    uint64_t peer_id;      // Идентификатор узла на уровне приложения
} endpoint_t;
```

---

### `conn_state_t`

```c
typedef enum {
    STATE_CONNECTING,    // Установка соединения
    STATE_AUTH_PENDING,  // Ожидание аутентификации
    STATE_KEY_EXCHANGE,  // Обмен ключами
    STATE_ESTABLISHED,   // Соединение установлено
    STATE_CLOSING,       // Закрытие
    STATE_BLOCKED,       // Заблокировано
    STATE_CLOSED         // Закрыто
} conn_state_t;
```

---

### `host_api_t`

Структура, передаваемая каждому плагину при инициализации. Даёт плагинам доступ к сервисам ядра.

```c
typedef struct {
    void*  internal_logger;   // Сырой spdlog::logger* — используется sync_plugin_context()
    void (*send)(const char* uri, uint32_t type,
                 const void* data, size_t size);
    handle_t (*create_connection)(const char* uri);
    void     (*close_connection)(handle_t handle);
    void     (*update_connection_state)(const char* uri, conn_state_t state);
    plugin_type_t plugin_type; // Устанавливается PluginManager перед вызовом init
} host_api_t;
```

`plugin_type` устанавливается в `PLUGIN_TYPE_HANDLER` или `PLUGIN_TYPE_CONNECTOR` до вызова точки входа — плагин может проверить, что загружен как правильный тип.

---

### `handler_t`

C-дескриптор, возвращаемый `handler_init`. `PluginManager` хранит указатель на него и вызывает его функции при диспетчеризации пакетов.

```c
typedef struct {
    void (*handle_message)(void* user_data, const header_t* header,
                           const endpoint_t* endpoint,
                           const void* payload, size_t payload_size);
    void (*handle_conn_state)(void* user_data, const char* uri, conn_state_t state);
    void (*shutdown)(void* user_data);
    uint32_t* supported_types;     // Массив принимаемых значений payload_type
    size_t    num_supported_types; // Длина массива (0 = принимать всё)
    void*     user_data;           // Передаётся первым аргументом в каждый коллбэк
} handler_t;
```

---

### `connector_ops_t`

```c
typedef struct {
    connection_ops_t* (*connect)(void* ctx, const char* uri);
    int  (*listen)(void* ctx, const char* host, uint16_t port);
    void (*get_scheme)(void* ctx, char* buf, size_t size); // напр. "tcp"
    void (*get_name)(void* ctx, char* buf, size_t size);   // напр. "TCP Connector"
    void (*shutdown)(void* ctx);
    void* connector_ctx;
} connector_ops_t;
```

---

## C++ классы (`include/`, `core/`, `sdk/cpp/`)

---

### `Logger`

**Заголовок:** `include/logger.hpp`  
**Реализация:** `src/logger.cpp`

Потокобезопасный логгер Meyers Singleton. Установите статические члены до первого вызова `LOG_*`; экземпляр создаётся лениво.

```cpp
class Logger {
public:
    // Конфигурация (установить до первого LOG_*)
    static std::string log_level;        // по умолчанию: "info"
    static std::string log_file;         // по умолчанию: "logs/goodnet.log"
    static size_t      max_size;         // по умолчанию: 10 МБ
    static int         max_files;        // по умолчанию: 5
    static std::string project_root;     // из CMake-define GOODNET_PROJECT_ROOT
    static bool        strip_extension;  // по умолчанию: false
    static int         source_detail_mode; // по умолчанию: 0 (авто)

    // Жизненный цикл
    static std::shared_ptr<spdlog::logger> get();
    static void shutdown();
    static void set_external_logger(std::shared_ptr<spdlog::logger> ext);

    // Логирование (используется через макросы, не напрямую)
    template<typename... Args>
    static void info(std::string_view file, int line,
                     fmt::format_string<Args...>, Args&&...);
    // ... trace, debug, warn, error, critical (та же сигнатура)
};
```

`spdlog/spdlog.h` **не** экспонируется через `logger.hpp`. Включается только `spdlog/common.h`. Это обеспечивает быструю компиляцию для всех TU, включающих заголовок логгера.

---

### `PluginManager`

**Заголовок:** `core/pluginManager.hpp`  
**Реализация:** `core/pluginManager_core.cpp`, `_state.cpp`, `_lookup.cpp`

Владеет жизненным циклом всех загруженных плагинов. Потокобезопасен: `std::shared_mutex` (чтение для поиска, запись для загрузки/выгрузки).

```cpp
class PluginManager {
public:
    explicit PluginManager(host_api_t* api, fs::path plugins_base_dir = "");
    ~PluginManager();  // вызывает unload_all()

    // Загрузка
    std::expected<void, std::string> load_plugin(const fs::path& path);
    void load_all_plugins();  // сканирует поддиректории handlers/ и connectors/

    // Поиск (shared lock — без копирования, безопасен для нескольких потоков)
    std::optional<handler_t*>       find_handler_by_name(std::string_view name) const;
    std::optional<connector_ops_t*> find_connector_by_scheme(std::string_view scheme) const;
    std::vector<handler_t*>         get_active_handlers() const;

    // Управление состоянием (unique lock)
    bool unload_handler (std::string_view name);
    bool enable_handler (std::string_view name);
    bool disable_handler(std::string_view name);
    void unload_all();

    // Статистика
    size_t get_enabled_handler_count() const;
    size_t get_enabled_connector_count() const;
    void   list_plugins() const;
};
```

Обнаружение плагина проверяет `.so.json`-манифест целостности перед вызовом `dlopen`. Плагины без корректного манифеста отклоняются.

---

### `gn::Signal<Args...>` / `PacketSignal`

**Заголовок:** `include/signals.hpp`

Типобезопасный сигнал на Boost.Asio для асинхронной доставки нескольким подписчикам через strand.

```cpp
template<typename... Args>
class Signal {
public:
    explicit Signal(boost::asio::io_context& ioc);

    template<typename Func>
    requires SignalHandler<Func, Args...>
    void connect(Func&& handler);

    void emit(Args... args);      // постит всех обработчиков в strand
    void disconnect_all();
    [[nodiscard]] size_t size() const;
};

// Конкретный тип для маршрутизации пакетов
using PacketSignal = Signal<
    const header_t*,
    const endpoint_t*,
    std::span<const char>
>;
```

`emit()` постит работу в `boost::asio::strand`. Обработчики выполняются последовательно в пуле потоков — нет гонок данных без явных блокировок в горячем пути.

---

### `gn::IHandler`

**Заголовок:** `sdk/cpp/handler.hpp`

C++ базовый класс для Handler-плагинов.

```cpp
class IHandler {
public:
    virtual void on_init()     {}
    virtual void on_shutdown() {}

    virtual void handle_message(
        const header_t*  header,
        const endpoint_t* endpoint,
        const void*      payload,
        size_t           payload_size) = 0;

    virtual void handle_connection_state(const char* uri, conn_state_t state) {}

    void set_supported_types(const std::vector<uint32_t>& types);
    handler_t* to_c_handler();  // вызывается макросом HANDLER_PLUGIN

protected:
    host_api_t* api_ = nullptr;
    void send(const char* uri, uint32_t type, const void* data, size_t size);
};
```

---

### `gn::IConnector` / `gn::IConnection`

**Заголовок:** `sdk/cpp/connector.hpp`

```cpp
class IConnector {
public:
    virtual void on_init()     {}
    virtual void on_shutdown() {}

    virtual std::unique_ptr<IConnection> create_connection(const std::string& uri) = 0;
    virtual bool        start_listening(const std::string& host, uint16_t port) = 0;
    virtual std::string get_scheme() const = 0;
    virtual std::string get_name()   const = 0;

    connector_ops_t* to_c_ops();  // вызывается макросом CONNECTOR_PLUGIN
protected:
    host_api_t* api_ = nullptr;
};

class IConnection {
public:
    virtual bool       do_send(const void* data, size_t size) = 0;
    virtual void       do_close() = 0;
    virtual bool       is_connected() const = 0;
    virtual endpoint_t get_remote_endpoint() const = 0;
    virtual std::string get_uri_string() const = 0;

    void notify_data(const void* data, size_t size);
    void notify_close();
    void notify_error(int error_code);

    connection_ops_t* to_c_ops();
};
```

---

### `gn::Stats`

**Заголовок:** `include/stats.hpp`

Плоская структура атомарных счётчиков рантайма. Все члены `static inline` — экземпляр не нужен.

```cpp
struct Stats {
    static inline std::atomic<size_t> packets_sent     = 0;
    static inline std::atomic<size_t> packets_received = 0;
    static inline std::atomic<size_t> connection_count = 0;
    static inline std::atomic<bool>   is_running       = false;
    static inline std::atomic<bool>   is_initialized   = false;
    static inline std::chrono::system_clock::time_point start_time;
    // ... полный список в заголовке
};
```

---

## Константы сообщений

```c
#define MSG_TYPE_SYSTEM       0
#define MSG_TYPE_AUTH         1
#define MSG_TYPE_KEY_EXCHANGE 2
#define MSG_TYPE_HEARTBEAT    3
#define MSG_TYPE_CHAT       100
#define MSG_TYPE_FILE       200

#define GNET_MAGIC   0x474E4554U  // "GNET" в ASCII
#define STATUS_OK    0
#define STATUS_ERROR 1
```
