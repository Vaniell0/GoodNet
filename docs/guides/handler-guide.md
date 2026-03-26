# Handler: гайд

Handler — плагин-обработчик сообщений. Получает расшифрованные пакеты через [SignalBus](../architecture/signal-bus.md), может отвечать, пересылать, broadcast.

См. также: [Connector: гайд](../guides/connector-guide.md) · [Система плагинов](../architecture/plugin-system.md) · [SignalBus](../architecture/signal-bus.md)

## Пошаговый гайд

### 1. Наследуемся от IHandler

```cpp
// plugins/handlers/echo/echo.cpp
#include <handler.hpp>

class EchoHandler final : public gn::IHandler {
    const char* get_plugin_name() const override { return "echo"; }

    void on_init() override {
        // Подписаться на конкретные типы сообщений.
        // Без вызова set_supported_types() — wildcard (все типы).
        set_supported_types({100, 200});
    }

    void handle_message(const header_t* hdr, const endpoint_t* ep,
                        std::span<const uint8_t> payload) override {
        // payload — уже расшифрованный, без header.
        // ep->peer_id — conn_id для ответа.
        // ep->address, ep->port — адрес пира.
        // ep->pubkey — Ed25519 user pubkey пира (после Noise_XX handshake).

        // Отправить payload обратно отправителю:
        send_response(ep->peer_id, hdr->payload_type, payload);

        // Или отправить по URI:
        // send("tcp://other-node:25565", 100, payload);

        // Или broadcast всем:
        // broadcast(hdr->payload_type, payload);
    }

    propagation_t on_result(const header_t*, uint32_t) override {
        // CONSUMED — остановить цепочку handlers, pin affinity.
        // CONTINUE — передать следующему handler.
        // REJECT — отбросить пакет.
        return PROPAGATION_CONSUMED;
    }

    void on_conn_state(const char* uri, conn_state_t state) override {
        // Уведомление о смене состояния соединения.
        // uri — hex pubkey пира.
        if (state == STATE_ESTABLISHED) { /* ... */ }
    }

    void on_shutdown() override {
        // Cleanup перед dlclose(). Обязательно освободить ресурсы.
    }
};

HANDLER_PLUGIN(EchoHandler)
```

### 2. Доступные helper-методы

IHandler предоставляет обёртки над `host_api_t`:

```cpp
// Отправка
send(uri, msg_type, payload_span);
send_response(conn_id, msg_type, payload_span);
broadcast(msg_type, payload_span);
disconnect(conn_id);

// Запросы
conn_id_t id = find_conn("hex_pubkey_64_chars");
endpoint_t ep; get_peer_info(id, ep);
std::string val = config_get("my_plugin.setting");

// Криптография
uint8_t sig[64]; sign(data, size, sig);
int ok = verify(data, size, pubkey, sig);

// Логирование
log(2, __FILE__, __LINE__, "info message");
// Или через LOG_INFO/LOG_WARN если handler.hpp подключает logger.hpp
```

## Dispatch chain

Handlers вызываются в порядке priority: **0 = highest** (вызывается первым), **255 = lowest** (последним). Priority задаётся через `plugin_info_t::priority`.

```
Пакет type=100:
  Handler "firewall"  (priority=0)   → CONTINUE     ← проверяет, пропускает
  Handler "echo"      (priority=128) → CONSUMED     ← обработал, СТОП
  Handler "logger"    (priority=200) ← НЕ ВЫЗЫВАЕТСЯ (chain остановлена)
```

`PROPAGATION_CONSUMED` также пинит [session affinity](../architecture/signal-bus.md#session-affinity): последующие пакеты с этого conn_id идут напрямую к "echo", минуя цепочку.

## Typed payloads (PodData\<T\>)

Для структурированных данных — zero-copy обёртка (`sdk/cpp/data.hpp`):

```cpp
#include <data.hpp>

struct ChatMsg {
    uint32_t room_id;
    uint32_t flags;
    char     text[256];
};

// Отправка:
gn::sdk::PodData<ChatMsg> msg;
msg.get().room_id = 42;
strncpy(msg.get().text, "hello", 256);
send_response(ep->peer_id, 100,
              std::span<const uint8_t>(msg.data(), msg.size()));

// Приём:
void handle_message(..., std::span<const uint8_t> payload) override {
    if (payload.size() < sizeof(ChatMsg)) return;
    auto* chat = reinterpret_cast<const ChatMsg*>(payload.data());
    // chat->room_id, chat->text ...
}
```

## Пример: Logger handler

`plugins/handlers/logger/logger.cpp` — ~100 строк:

```cpp
class MsgLogger final : public gn::IHandler {
    const char* get_plugin_name() const override { return "MsgLogger"; }

    void on_init() override {
        set_supported_types({1, 2, 3, 4, 10, 11, 100, 200});
        start_ts_ = now_us();
    }

    void handle_message(const header_t* hdr, const endpoint_t* ep,
                        std::span<const uint8_t> payload) override {
        uint64_t n = ++count_;
        char pk[9] = "????????";
        if (ep) snprintf(pk, 9, "%02x%02x%02x%02x",
                         ep->pubkey[0], ep->pubkey[1],
                         ep->pubkey[2], ep->pubkey[3]);
        LOG_INFO("[MsgLogger] #{} type={} from={}:{} size={} pk={}...",
                 n, hdr->payload_type, ep->address, ep->port,
                 payload.size(), pk);
    }
    // on_result() не переопределён → PROPAGATION_CONTINUE (по умолчанию)
    // Не блокирует цепочку — другие handlers тоже получат пакет.
};

HANDLER_PLUGIN(gn::MsgLogger)
```

## Error handling patterns

Handler должен быть устойчивым к некорректным данным и неожиданным состояниям.

### Validation

```cpp
void handle_message(const header_t* hdr, const endpoint_t* ep,
                    std::span<const uint8_t> payload) override {
    // 1. Проверить размер перед cast
    if (payload.size() < sizeof(ExpectedStruct)) {
        LOG_WARN("Payload too small: {} < {}", payload.size(), sizeof(ExpectedStruct));
        return;  // или PROPAGATION_REJECT в on_result()
    }

    // 2. Проверить ep не nullptr (может быть если connection уже closed)
    if (!ep) {
        LOG_ERROR("Received message with null endpoint");
        return;
    }

    auto* msg = reinterpret_cast<const ExpectedStruct*>(payload.data());

    // 3. Проверить поля структуры
    if (msg->room_id == 0 || msg->room_id > MAX_ROOMS) {
        LOG_WARN("Invalid room_id: {}", msg->room_id);
        return;
    }

    // 4. Обработка успешна
    process_message(msg, ep);
}
```

### Exception safety

**НЕ бросайте exceptions из handle_message()** — это вызывается из Core IO thread. Необработанный exception может убить весь процесс.

```cpp
void handle_message(const header_t* hdr, const endpoint_t* ep,
                    std::span<const uint8_t> payload) override {
    try {
        // Ваша логика
        risky_operation(payload);
    } catch (const std::exception& e) {
        LOG_ERROR("Exception in handler: {}", e.what());
        // НЕ re-throw, просто логируем и возвращаемся
    }
}
```

### Send failure handling

`send()` и `broadcast()` могут вернуть false если connection не в STATE_ESTABLISHED или очередь переполнена:

```cpp
bool sent = send_response(ep->peer_id, 200, response_data);
if (!sent) {
    LOG_WARN("Failed to send response to conn {}", ep->peer_id);
    // Можно отложить retry или disconnect peer
}
```

## Memory ownership rules

### Pointer lifetime (критично!)

**ep->pubkey**, **ep->address**, **ep->port** валидны **только внутри handle_message()**. Указатель `ep` ссылается на `ConnectionRecord` в [RCU registry](../architecture/connection-manager.md#rcu-registry) — после выхода из функции запись может быть удалена.

**❌ НЕПРАВИЛЬНО:**
```cpp
class MyHandler : public IHandler {
    const endpoint_t* saved_ep_;  // ОПАСНО!

    void handle_message(const header_t* hdr, const endpoint_t* ep, ...) override {
        saved_ep_ = ep;  // ❌ use-after-free при disconnect
    }

    void some_later_function() {
        send_response(saved_ep_->peer_id, ...);  // ❌ CRASH!
    }
};
```

**✅ ПРАВИЛЬНО:**
```cpp
class MyHandler : public IHandler {
    std::unordered_map<conn_id_t, UserPubkey> active_peers_;

    void handle_message(const header_t* hdr, const endpoint_t* ep, ...) override {
        // Копируем данные, а не указатель
        UserPubkey pubkey;
        std::memcpy(pubkey.data(), ep->pubkey, 32);
        active_peers_[ep->peer_id] = pubkey;
    }

    void some_later_function(conn_id_t id) {
        auto it = active_peers_.find(id);
        if (it != active_peers_.end()) {
            // Используем скопированные данные
        }
    }
};
```

### Payload lifetime

`payload` span валиден только внутри handle_message(). Если нужно сохранить данные — копируйте:

```cpp
void handle_message(..., std::span<const uint8_t> payload) override {
    // ❌ ОПАСНО: сохранить span
    // saved_span_ = payload;  // use-after-free!

    // ✅ ПРАВИЛЬНО: скопировать данные
    std::vector<uint8_t> data_copy(payload.begin(), payload.end());
    pending_messages_.push_back(std::move(data_copy));
}
```

## Disconnection handling

`on_conn_state()` вызывается при смене состояния соединения, включая **неожиданный disconnect** (timeout, reset, ошибка шифрования).

### Cleanup per-connection state

```cpp
class RoomHandler : public IHandler {
    // Карта: conn_id → room_id
    std::unordered_map<conn_id_t, uint32_t> participant_rooms_;
    // Карта: room_id → set<conn_id>
    std::unordered_map<uint32_t, std::unordered_set<conn_id_t>> room_participants_;

    void handle_message(const header_t* hdr, const endpoint_t* ep,
                        std::span<const uint8_t> payload) override {
        auto* msg = reinterpret_cast<const JoinRoomMsg*>(payload.data());

        // Participant присоединяется к комнате
        participant_rooms_[ep->peer_id] = msg->room_id;
        room_participants_[msg->room_id].insert(ep->peer_id);
    }

    void on_conn_state(const char* uri, conn_state_t state) override {
        if (state == STATE_CLOSED) {
            // КРИТИЧНО: cleanup при disconnect
            conn_id_t id = parse_conn_id_from_uri(uri);

            auto it = participant_rooms_.find(id);
            if (it != participant_rooms_.end()) {
                uint32_t room_id = it->second;

                // Удалить из комнаты
                room_participants_[room_id].erase(id);
                if (room_participants_[room_id].empty()) {
                    room_participants_.erase(room_id);
                }

                // Удалить из карты участников
                participant_rooms_.erase(it);

                LOG_INFO("Participant {} left room {}", id, room_id);
            }
        }
    }
};
```

### Broadcast to room example

```cpp
void broadcast_to_room(uint32_t room_id, std::span<const uint8_t> msg) {
    auto it = room_participants_.find(room_id);
    if (it == room_participants_.end()) return;

    for (conn_id_t peer_id : it->second) {
        bool sent = send_response(peer_id, MSG_TYPE_ROOM_CHAT, msg);
        if (!sent) {
            LOG_WARN("Failed to send to peer {} in room {}", peer_id, room_id);
            // Peer может уже disconnect — игнорируем
        }
    }
}
```

---

**См. также:** [Connector: гайд](../guides/connector-guide.md) · [Система плагинов](../architecture/plugin-system.md) · [SignalBus](../architecture/signal-bus.md) · [Wire format](../protocol/wire-format.md)

## Real-world пример: MsgLogger

Встроенный плагин `plugins/handlers/logger/logger.cpp` — production-ready handler:

```cpp
class MsgLogger final : public IHandler {
    std::atomic<uint64_t> count_{0};
    uint64_t start_ts_;

    const char* get_plugin_name() const override { return "MsgLogger"; }

    void on_init() override {
        // Wildcard: receive every message type
        set_supported_types({1, 2, 3, 4, 10, 11, 100, 200});
        start_ts_ = now_us();
        LOG_INFO("[MsgLogger] started — logging all packet types");
    }

    void handle_message(const header_t* header,
                        const endpoint_t* endpoint,
                        std::span<const uint8_t> payload) override
    {
        if (!header) return;

        const uint64_t count = ++count_;  // atomic

        // Peer pubkey prefix (first 4 bytes → 8 hex chars)
        char pk_prefix[9] = "????????";
        if (endpoint) {
            std::snprintf(pk_prefix, sizeof(pk_prefix),
                          "%02x%02x%02x%02x",
                          endpoint->pubkey[0], endpoint->pubkey[1],
                          endpoint->pubkey[2], endpoint->pubkey[3]);
        }

        LOG_INFO("[MsgLogger] #{} type={} from={}:{} size={} pk={}...",
                 count,
                 header->payload_type,
                 endpoint ? endpoint->address : "?",
                 endpoint ? endpoint->port : 0,
                 payload.size(),
                 pk_prefix);
    }

    void on_shutdown() override {
        const uint64_t elapsed_ms = (now_us() - start_ts_) / 1000;
        LOG_INFO("[MsgLogger] stopped — {} packets logged in {} ms",
                 count_.load(), elapsed_ms);
    }

    propagation_t on_result(const header_t*, uint32_t) override {
        return PROPAGATION_CONTINUE;  // Не блокируем chain
    }
};

HANDLER_PLUGIN(MsgLogger)
```

**Ключевые детали:**
- **Wildcard subscription**: `set_supported_types()` со всеми типами
- **Zero payload access**: логирует только header + endpoint (не копирует payload)
- **Atomic counter**: `std::atomic<uint64_t>` для thread-safe count
- **PROPAGATION_CONTINUE**: не блокирует другие handlers
- **Lifetime tracking**: `start_ts_` → вычисление elapsed_ms в on_shutdown

