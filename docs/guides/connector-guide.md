# Connector: гайд

Connector — плагин-транспорт. Отвечает за сетевой ввод-вывод: TCP, ICE/DTLS, UDP — любой протокол. Core управляет handshake, шифрованием и dispatch; connector только доставляет байты.

См. также: [Handler: гайд](../guides/handler-guide.md) · [Система плагинов](../architecture/plugin-system.md) · [ConnectionManager](../architecture/connection-manager.md)

## Пошаговый гайд

### 1. Наследуемся от IConnector

```cpp
#include <connector.hpp>

class UdpConnector final : public gn::IConnector {
    std::string get_scheme() const override { return "udp"; }
    std::string get_name()   const override { return "UDP Transport"; }

    void on_init() override {
        // Создать io_context, потоки, etc.
    }

    int do_connect(const char* uri) override {
        // Распарсить URI, установить соединение.
        // При успехе вызвать:
        endpoint_t ep{};
        strncpy(ep.address, host, sizeof(ep.address));
        ep.port = port;
        conn_id_t id = notify_connect(&ep);
        // Сохранить id для последующих операций.
        return 0;
    }

    int do_listen(const char* host, uint16_t port) override {
        // bind, listen. При каждом новом входящем соединении:
        // conn_id_t id = notify_connect(&ep);
        return 0;
    }

    int do_send(conn_id_t id, std::span<const uint8_t> data) override {
        // Отправить data на socket, ассоциированный с id.
        // return 0 = OK, -1 = error.
        return 0;
    }

    int do_send_gather(conn_id_t id, const struct iovec* iov, int n) override {
        // Vectored send (writev). Опционально — дефолт вызывает do_send() в цикле.
        return IConnector::do_send_gather(id, iov, n);
    }

    void do_close(conn_id_t id, bool hard) override {
        // Закрыть socket.
        // ОБЯЗАТЕЛЬНО вызвать:
        notify_disconnect(id, 0);
    }

    void on_shutdown() override {
        // Cleanup: закрыть все sockets, остановить потоки.
    }
};

CONNECTOR_PLUGIN(UdpConnector)
```

## Контракт connector ↔ core

Connector **вызывает** (через helpers):
- `notify_connect(&ep)` → получает `conn_id_t` от ядра
- `notify_data(id, raw_bytes)` → ядро делает [framing, decrypt, dispatch](../architecture/connection-manager.md#dispatch-path)
- `notify_disconnect(id, error)` → ядро удаляет ConnectionRecord

Ядро **вызывает** (через `connector_ops_t`):
- `connect(ctx, uri)` → async outgoing connection
- `listen(ctx, host, port)` → start accepting inbound connections
- `send_to(ctx, id, data, size)` → connector отправляет bytes
- `send_gather(ctx, id, iov, count)` → vectored send (опционально, NULL = fallback на send_to)
- `close(ctx, id)` → graceful close (drain pending writes)
- `close_now(ctx, id)` → hard close без drain (shutdown / error recovery)
- `get_scheme(ctx, buf, size)` → URI scheme ("tcp", "udp", "ice", ...)
- `get_name(ctx, buf, size)` → human-readable имя для логов
- `shutdown(ctx)` → полная остановка

Connector также может вызывать:
- `add_transport(ctx, pubkey_hex, ep, scheme)` → зарегистрировать дополнительный транспорт для уже ESTABLISHED пира (multipath)

## EP_FLAG_TRUSTED

Если connector знает, что соединение доверенное (loopback, veth, unix socket), он ставит `ep.flags = EP_FLAG_TRUSTED` при `notify_connect()`. Ядро установит `is_localhost = true` на ConnectionRecord и будет принимать plaintext ([GNET_FLAG_TRUSTED](../protocol/wire-format.md)) фреймы.

TCP connector делает это автоматически для `127.x.x.x` и `::1`:
```cpp
if (remote_endpoint.address().is_loopback())
    ep.flags = EP_FLAG_TRUSTED;
```

## Threading и io_context rules

### Connector может иметь свой io_context

Connector **не обязан** использовать Core io_context. Например, TCP connector создаёт свой `boost::asio::io_context` + отдельный пул потоков:

```cpp
class TcpConnector : public IConnector {
    boost::asio::io_context ioc_;
    std::vector<std::thread> threads_;

    void on_init() override {
        // Запустить свои IO потоки
        for (int i = 0; i < 4; ++i) {
            threads_.emplace_back([this]() { ioc_.run(); });
        }
    }

    void on_shutdown() override {
        ioc_.stop();
        for (auto& t : threads_) t.join();
    }
};
```

**Почему отдельный io_context?**
- Изоляция: connector crash не убивает Core
- Tuning: connector может иметь свой thread pool size
- Flexibility: connector может использовать другой event loop (libuv, GLib, etc.)

### notify_* callbacks thread-safe

`notify_connect()`, `notify_data()`, `notify_disconnect()` **thread-safe** — можно вызывать из connector потока или Core потока.

```cpp
// Connector thread (boost::asio handler):
void handle_accept(boost::system::error_code ec, tcp::socket socket) {
    if (!ec) {
        endpoint_t ep = make_endpoint(socket.remote_endpoint());
        conn_id_t id = notify_connect(&ep);  // ✅ thread-safe
        start_read(id, std::move(socket));
    }
}
```

Внутри Core callbacks используют lock/atomic для синхронизации с [RCU registry](../architecture/connection-manager.md#rcu-registry).

### do_send() может вызываться из Core thread

Core вызывает `send_to(ctx, id, data, size)` из своего IO thread. Connector должен быть thread-safe или использовать strand:

```cpp
int do_send(conn_id_t id, std::span<const uint8_t> data) override {
    auto it = sockets_.find(id);
    if (it == sockets_.end()) return -1;

    // ✅ ПРАВИЛЬНО: post в connector io_context через strand
    boost::asio::post(strand_, [id, data_copy = std::vector(data.begin(), data.end())]() {
        // Теперь в connector thread, безопасно работать с socket
        async_write(socket, boost::asio::buffer(data_copy), ...);
    });

    return 0;
}
```

## Error recovery

Connector должен различать **hard errors** (connection reset, неустранимая ошибка) и **soft errors** (временная перегрузка, retry возможен).

### Hard errors → notify_disconnect немедленно

```cpp
void handle_read(conn_id_t id, boost::system::error_code ec, size_t bytes) {
    if (ec == boost::asio::error::connection_reset ||
        ec == boost::asio::error::eof ||
        ec == boost::asio::error::broken_pipe) {
        // Hard error: соединение мертво
        notify_disconnect(id, static_cast<int>(ec.value()));
        sockets_.erase(id);
        return;
    }

    if (ec) {
        LOG_ERROR("Unexpected read error on conn {}: {}", id, ec.message());
        notify_disconnect(id, static_cast<int>(ec.value()));
        sockets_.erase(id);
        return;
    }

    // Успешное чтение
    notify_data(id, buffer.data(), bytes);
}
```

### Soft errors → retry

```cpp
void handle_write(conn_id_t id, boost::system::error_code ec, size_t bytes) {
    if (ec == boost::asio::error::would_block ||
        ec == boost::asio::error::try_again) {
        // Soft error: retry
        LOG_DEBUG("Socket {} would block, retrying later", id);
        retry_write(id);
        return;
    }

    if (ec) {
        // Hard error
        notify_disconnect(id, static_cast<int>(ec.value()));
        return;
    }

    // Успех
}
```

### Protocol errors → close_now

Если connector получает некорректные данные (bad magic, proto_ver mismatch), Core вызовет `close_now(id, true)`:

```cpp
void do_close(conn_id_t id, bool hard) override {
    auto it = sockets_.find(id);
    if (it == sockets_.end()) return;

    if (hard) {
        // Hard close: немедленно закрыть socket без graceful shutdown
        boost::system::error_code ec;
        it->second.close(ec);
    } else {
        // Graceful close: дождаться отправки pending data
        it->second.shutdown(tcp::socket::shutdown_both);
    }

    sockets_.erase(it);
    notify_disconnect(id, 0);
}
```

## Backpressure flow: Core → Connector

Когда [PerConnQueue](../architecture/connection-manager.md#send-path) достигает 8 MB limit, Core **перестаёт вызывать** `send_to()` для этого conn_id. Connector должен обработать ситуацию:

### Connector recv buffer может переполниться

```
 Core:       PerConnQueue full (8 MB) → stop calling send_to()
              ↓
 Connector:  Outbound queue пустая → нет отправки
              ↓
             Inbound data продолжает приходить → recv buffer растёт
              ↓
             Connector должен применить flow control
```

### Решение: Slow down accept() или drop inbound

```cpp
void handle_accept(boost::system::error_code ec, tcp::socket socket) {
    // Проверить: есть ли capacity для новых соединений?
    if (active_connections_.size() >= MAX_CONNECTIONS) {
        LOG_WARN("Connection limit reached, rejecting new connection");
        socket.close();  // Drop connection
        schedule_accept();  // Продолжить listen
        return;
    }

    // Accept
    conn_id_t id = notify_connect(...);
    active_connections_.insert(id);
    start_read(id, std::move(socket));
    schedule_accept();
}
```

### Альтернатива: Pause accept() временно

```cpp
if (active_connections_.size() >= MAX_CONNECTIONS) {
    // НЕ вызываем schedule_accept() → acceptor приостановлен
    LOG_INFO("Pausing accept() due to backpressure");

    // Через некоторое время (когда connections freed) возобновить:
    backpressure_timer_.expires_after(std::chrono::seconds(5));
    backpressure_timer_.async_wait([this](auto ec) {
        if (!ec) schedule_accept();
    });
    return;
}
```

## TCP connector

`plugins/connectors/tcp/tcp.cpp` (~430 строк) — полноценная реализация на Boost.Asio:

- Свой `io_context` + пул потоков (не зависит от Core io_context)
- [Двухфазное чтение](../protocol/wire-format.md#tcp-framing): header(20) → payload(N) → zero-copy notify_data
- Write pipeline: deque → drain batch (64 frames) → async_write с ConstBufferSequence (→ writev)
- Protocol error → close + notify_disconnect

---

**См. также:** [Handler: гайд](../guides/handler-guide.md) · [Система плагинов](../architecture/plugin-system.md) · [ConnectionManager](../architecture/connection-manager.md) · [Wire format](../protocol/wire-format.md)
