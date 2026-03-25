# Best Practices

Проверенные практики разработки на GoodNet: производительность, безопасность, отказоустойчивость.

См. также: [Core Concepts](data/projects/GoodNet/docs/guides/concepts.md) · [Handler гайд](data/projects/GoodNet/docs/guides/handler-guide.md) · [Architecture](../architecture.md)

## Handler разработка

### 1. Используйте правильный propagation

```cpp
// ❌ WRONG: CONSUMED для stateless handlers
class MetricsHandler : public IHandler {
    propagation_t on_result(...) override {
        return PROPAGATION_CONSUMED;  // Блокирует других!
    }
};

// ✅ CORRECT: CONTINUE для observability
class MetricsHandler : public IHandler {
    propagation_t on_result(...) override {
        return PROPAGATION_CONTINUE;  // Позволяет другим обработать
    }
};
```

**Правило:**
- **CONSUMED**: stateful protocols (chat sessions, file transfer) — нужен session affinity
- **CONTINUE**: observability (logger, metrics, audit) — не блокируем других
- **REJECT**: validation errors — останавливаем chain без affinity

### 2. Копируйте данные из endpoint_t

```cpp
// ❌ WRONG: сохранение указателя
class ChatHandler : public IHandler {
    const endpoint_t* current_peer_;  // Dangling pointer после handle_message!

    void handle_message(..., const endpoint_t* ep, ...) override {
        current_peer_ = ep;  // use-after-free!
    }
};

// ✅ CORRECT: копирование нужных полей
class ChatHandler : public IHandler {
    struct PeerInfo {
        conn_id_t peer_id;
        std::array<uint8_t, 32> user_pubkey;
        std::string address;
    };
    std::unordered_map<conn_id_t, PeerInfo> peers_;

    void handle_message(..., const endpoint_t* ep, ...) override {
        PeerInfo info;
        info.peer_id = ep->peer_id;
        std::memcpy(info.user_pubkey.data(), ep->user_pubkey, 32);
        info.address = ep->address;
        peers_[ep->peer_id] = std::move(info);
    }
};
```

### 3. Обрабатывайте disconnection

```cpp
class ChatHandler : public IHandler {
    std::unordered_map<conn_id_t, UserState> active_users_;

    void on_conn_state(std::string_view uri, conn_state_t state) override {
        if (state == STATE_CLOSED) {
            // Cleanup: найти conn_id по URI, удалить state
            for (auto it = active_users_.begin(); it != active_users_.end(); ) {
                if (it->second.uri == uri) {
                    LOG_INFO("User {} disconnected, cleaning up", it->first);
                    it = active_users_.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }
};
```

### 4. Валидируйте payload

```cpp
void handle_message(..., std::span<const uint8_t> payload) override {
    // Проверка минимального размера
    if (payload.size() < sizeof(MyProtocolHeader)) {
        LOG_WARN("Payload too small: {} < {}",
                 payload.size(), sizeof(MyProtocolHeader));
        return PROPAGATION_REJECT;
    }

    // Zero-copy десериализация
    auto msg = sdk::PodData<MyProtocolHeader>::from_span(payload);
    if (!msg) {
        LOG_WARN("Invalid payload format");
        return PROPAGATION_REJECT;
    }

    // Валидация полей
    if (msg.get()->version != PROTOCOL_VERSION) {
        LOG_WARN("Unsupported protocol version: {}", msg.get()->version);
        return PROPAGATION_REJECT;
    }

    // OK, обрабатываем
    process_message(*msg.get());
    return PROPAGATION_CONSUMED;
}
```

## Performance tips

### 1. Минимизируйте аллокации

```cpp
// ❌ SLOW: аллокация на каждый пакет
void handle_message(..., std::span<const uint8_t> payload) override {
    std::vector<uint8_t> copy(payload.begin(), payload.end());
    process(copy);
}

// ✅ FAST: zero-copy или reuse buffer
class MyHandler : public IHandler {
    std::vector<uint8_t> work_buf_;  // Переиспользуется

    void handle_message(..., std::span<const uint8_t> payload) override {
        // Option 1: zero-copy если возможно
        process_view(payload);

        // Option 2: reuse buffer если нужна копия
        work_buf_.assign(payload.begin(), payload.end());
        process(work_buf_);
    }
};
```

### 2. Batching для send

```cpp
// ❌ SLOW: много мелких send() → много syscalls
for (auto& peer : peers) {
    send(peer.uri, MSG_TYPE_SYNC, small_data);  // N syscalls
}

// ✅ FAST: batch в один пакет
std::vector<uint8_t> batch;
for (auto& peer : peers) {
    append_to_batch(batch, peer.id, small_data);
}
broadcast(MSG_TYPE_SYNC_BATCH, batch);  // 1 syscall per connector
```

### 3. Используйте session affinity

```cpp
// Первый пакет: установить affinity
void handle_message(...) override {
    if (is_first_packet_in_session()) {
        // Установить session state
        sessions_[ep->peer_id] = SessionState{...};
        return PROPAGATION_CONSUMED;  // Pin affinity
    }

    // Последующие пакеты: прямой вызов (skip chain)
    auto& session = sessions_[ep->peer_id];
    session.process(payload);
    return PROPAGATION_CONSUMED;
}
```

**Gain:** ~30x для high-frequency protocols.

## Security best practices

### 1. Verify peer identity

```cpp
void handle_message(..., const endpoint_t* ep, ...) override {
    // Проверить что peer authenticated
    if (!ep || ep->user_pubkey[0] == 0) {
        LOG_WARN("Unauthenticated peer — reject");
        return PROPAGATION_REJECT;
    }

    // Whitelist проверка
    if (!is_allowed_peer(ep->user_pubkey)) {
        LOG_WARN("Peer not in whitelist");
        return PROPAGATION_REJECT;
    }

    // OK, обрабатываем
    process_authenticated(ep, payload);
}
```

### 2. Rate limiting

```cpp
class MyHandler : public IHandler {
    struct RateLimit {
        std::chrono::steady_clock::time_point last_msg;
        uint32_t count_in_window;
    };
    std::unordered_map<conn_id_t, RateLimit> limits_;

    void handle_message(..., const endpoint_t* ep, ...) override {
        auto& limit = limits_[ep->peer_id];
        auto now = std::chrono::steady_clock::now();

        // Reset window каждую секунду
        if (now - limit.last_msg > std::chrono::seconds(1)) {
            limit.count_in_window = 0;
            limit.last_msg = now;
        }

        // Проверка лимита (100 msg/sec)
        if (++limit.count_in_window > 100) {
            LOG_WARN("Rate limit exceeded for peer {}", ep->peer_id);
            return PROPAGATION_REJECT;
        }

        process(payload);
    }
};
```

### 3. Sanitize user input

```cpp
void handle_chat_message(std::string_view text) {
    // Проверка длины
    if (text.size() > MAX_CHAT_LENGTH) {
        LOG_WARN("Chat message too long: {}", text.size());
        return;
    }

    // Проверка на control characters
    for (char c : text) {
        if (c < 0x20 && c != '\n' && c != '\t') {
            LOG_WARN("Invalid control character in chat");
            return;
        }
    }

    // OK, обрабатываем
    broadcast_chat(text);
}
```

## Error handling

### 1. Graceful degradation

```cpp
void handle_message(...) override {
    try {
        process_message(payload);
    } catch (const std::exception& e) {
        // Логируем, но не крашим handler
        LOG_ERROR("Error processing message: {}", e.what());
        return PROPAGATION_REJECT;
    }
}
```

### 2. Проверяйте send() результат

```cpp
// ❌ WRONG: игнорируем backpressure
send(uri, type, data);  // Может вернуть false!

// ✅ CORRECT: обрабатываем failure
if (!send(uri, type, data)) {
    LOG_WARN("Send failed (backpressure or invalid URI)");

    // Варианты:
    // 1. Queue локально → retry позже
    pending_messages_.push_back({uri, type, data});

    // 2. Drop с уведомлением
    notify_send_failed(uri);

    // 3. Fallback на другой path
    try_alternative_route(uri, type, data);
}
```

## Testing

### 1. Mock connector для unit tests

```cpp
TEST(MyHandlerTest, HandlesDisconnect) {
    Core core;
    auto handler = std::make_unique<MyHandler>();
    core.register_handler(handler.get());

    // Simulate connect
    endpoint_t ep{};
    ep.peer_id = 123;
    handler->on_conn_state("tcp://test:1234", STATE_ESTABLISHED);

    // Simulate disconnect
    handler->on_conn_state("tcp://test:1234", STATE_CLOSED);

    // Verify cleanup
    EXPECT_EQ(handler->active_users().size(), 0);
}
```

### 2. Stress test с backpressure

```cpp
TEST(MyHandlerTest, HandlesBackpressure) {
    // Отправить много данных → проверить что handler корректно обрабатывает
    for (int i = 0; i < 10000; ++i) {
        bool ok = send(uri, type, large_data);
        if (!ok) {
            // Backpressure triggered — OK, ждём drain
            std::this_thread::sleep_for(10ms);
            --i;  // Retry
        }
    }
}
```

---

**См. также:** [Handler гайд](data/projects/GoodNet/docs/guides/handler-guide.md) · [Core Concepts](data/projects/GoodNet/docs/guides/concepts.md) · [Performance optimization](../architecture/connection-manager.md#performance-optimizations)
