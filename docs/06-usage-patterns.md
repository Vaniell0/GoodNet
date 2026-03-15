# 06 — Методы применения

Практические примеры использования GoodNet: от простого сервера до relay mesh и ICE NAT traversal.

---

## Сервер

Минимальный сервер, слушающий TCP и логирующий входящие сообщения:

```cpp
#include <core.hpp>

int main() {
    gn::CoreConfig cfg;
    cfg.plugins.auto_load = true;
    cfg.plugins.dirs = {"./result/plugins"};
    cfg.network.io_threads = 4;

    gn::Core core(cfg);
    core.run_async();

    // Начать слушать
    if (auto tcp = core.pm().find_connector_by_scheme("tcp"))
        (*tcp)->listen((*tcp)->connector_ctx, "0.0.0.0", 25565);

    // Подписка на все типы сообщений
    core.subscribe_wildcard("my_server",
        [](auto name, auto hdr, auto ep, auto data) {
            fmt::print("[{}:{}] type={} size={}\n",
                       ep->address, ep->port,
                       hdr->payload_type, data->size());
            return PROPAGATION_CONSUMED;
        });

    // Ждать SIGINT
    std::signal(SIGINT, [](int) { /* stop */ });
    pause();
    core.stop();
}
```

CLI-эквивалент:

```bash
./result/bin/goodnet --listen 25565
```

---

## Клиент

Подключение к серверу и отправка данных:

```cpp
gn::Core core(cfg);
core.run_async();

// Инициировать подключение (handshake автоматический)
std::string target = "tcp://192.168.1.2:25565";

// Ждать handshake (busy-wait, как в main.cpp)
while (running) {
    core.send(target.c_str(), 0, nullptr, 0);  // trigger connect
    for (const auto& u : core.active_uris())
        if (u.find(target) != std::string::npos)
            goto connected;
    std::this_thread::sleep_for(100ms);
}

connected:
// Отправить данные
std::vector<uint8_t> payload(64 * 1024);  // 64 KB
core.send(target.c_str(), 1000, payload.data(), payload.size());
```

CLI-эквивалент:

```bash
./result/bin/goodnet --target tcp://192.168.1.2:25565 --size 64 -j 4
```

---

## Benchmark

Многопоточный бенчмарк из `main.cpp`:

```bash
# Сервер:
./result/bin/goodnet --listen 25565

# Клиент (4 потока, 64 KB пакеты):
./result/bin/goodnet --target tcp://server:25565 --size 64 -j 4

# CI-режим (10 секунд, exit code):
./result/bin/goodnet --target tcp://server:25565 \
    --exit-after 10 --exit-code

# Dashboard refresh 5 Hz:
./result/bin/goodnet --target tcp://server:25565 --hz 5
```

Dashboard показывает в реальном времени: throughput (Gbps), packets/sec, latency percentiles (p50/p95/p99), connection count.

---

## Подписки

### Конкретный тип

```cpp
core.subscribe(1000, "chat_listener",
    [](auto name, auto hdr, auto ep, auto data) -> propagation_t {
        process_chat(ep->peer_id, *data);
        return PROPAGATION_CONSUMED;
    }, 200 /*priority*/);
```

### Wildcard (все типы)

```cpp
core.subscribe_wildcard("debug_logger",
    [](auto name, auto hdr, auto ep, auto data) -> propagation_t {
        LOG_DEBUG("type={} from={}", hdr->payload_type, ep->address);
        return PROPAGATION_CONTINUE;  // не поглощать
    });
```

### Приоритеты

```
priority=255  security_handler    →  CONTINUE (пропустить)
priority=200  chat_handler        →  CONSUMED (обработать)
priority=  0  logger              →  не вызывается
```

### Отписка

```cpp
uint64_t sub_id = core.subscribe(1000, "temp", handler);
// ...
core.unsubscribe(sub_id);
```

---

## Relay mesh

Топология: hub + клиенты. Hub пересылает пакеты между клиентами.

```
Client B ──TCP──▶ Hub ◀──TCP── Client C
                   │
             relay forward
```

Hub не нуждается в специальном коде — relay обрабатывается ядром автоматически при наличии `CORE_CAP_RELAY`:

```cpp
// Клиент: отправить через relay
gn::msg::RelayPayload relay;
relay.ttl = 3;
memcpy(relay.dest_pubkey, target_pubkey, 32);
// + inner_frame (encrypted payload для получателя)
core.send("tcp://hub:25565", MSG_TYPE_RELAY, &relay, sizeof(relay) + inner_size);
```

Hub автоматически:
1. Декремент TTL
2. Поиск `dest_pubkey` в таблице соединений
3. Forward если найден, broadcast (gossip) если нет
4. Дедупликация по `packet_id`

---

## ICE NAT traversal

ICE позволяет установить прямой UDP-канал через NAT. Требует начальное TCP-соединение для SDP signaling.

### CLI

```bash
# С дефолтным STUN (stun.l.google.com:19302):
./result/bin/goodnet --target tcp://peer:25565 --ice-upgrade

# С локальным STUN (coturn):
./result/bin/goodnet --target tcp://peer:25565 --ice-upgrade \
    --config ci/config-ice.json
```

### Конфиг (`ci/config-ice.json`)

```json
{
  "ice.stun_server": "172.20.0.100",
  "ice.stun_port": "3478"
}
```

### Программно

```cpp
core.run_async();

// 1. Установить TCP-соединение
core.send("tcp://peer:25565", 0, nullptr, 0);
// ... ждать ESTABLISHED ...

// 2. Получить pubkey пира
auto pk = core.peer_pubkey(conn_id);
std::string hex = bytes_to_hex(pk.data(), pk.size());

// 3. Инициировать ICE
core.connect("ice://" + hex);
// ICE connector: OFFER → TCP → peer → ANSWER → TCP → ICE check → UDP
```

### Последовательность

```
1. TCP handshake → ESTABLISHED
2. core.connect("ice://peer_hex")
3. ICE connector генерирует local SDP (OFFER)
4. Отправляет IceSignalPayload через TCP
5. Пир генерирует ANSWER → отправляет через TCP
6. ICE connectivity checks (STUN)
7. DTLS handshake
8. Прямой UDP канал установлен
```

---

## Минимальный хендлер-плагин

```cpp
// plugins/handlers/echo/echo.cpp
#include <sdk/cpp/handler.hpp>

class EchoHandler : public gn::IHandler {
public:
    const char* get_plugin_name() const override { return "echo"; }

    const plugin_info_t* get_plugin_info() const override {
        static plugin_info_t info{"echo", 0x00010000, 128, {}, 0};
        return &info;
    }

    void on_init() override {
        set_supported_types({1000});  // пользовательский тип
    }

    void handle_message(const header_t* hdr, const endpoint_t* ep,
                         const void* payload, size_t size) override {
        // Echo обратно отправителю
        send_response(ep->peer_id, hdr->payload_type, payload, size);
    }

    propagation_t on_result(const header_t*, uint32_t) override {
        return PROPAGATION_CONSUMED;
    }

    void on_shutdown() override { }
};

HANDLER_PLUGIN(EchoHandler)
```

Сборка:

```cmake
add_library(echo SHARED echo.cpp)
target_link_libraries(echo PRIVATE goodnet_core)
```

---

## Мониторинг

### StatsSnapshot

```cpp
auto snap = core.stats_snapshot();

fmt::print("Connections: {}\n", snap.connections);
fmt::print("RX: {} bytes, {} packets\n", snap.rx_bytes, snap.rx_packets);
fmt::print("TX: {} bytes, {} packets\n", snap.tx_bytes, snap.tx_packets);
fmt::print("Auth: {} ok, {} fail\n", snap.auth_ok, snap.auth_fail);
fmt::print("Dispatch latency avg: {} ns\n", snap.dispatch_latency.avg_ns());
```

### LatencyHistogram

```cpp
auto& lat = snap.dispatch_latency;
// Бакеты: <1μs, <10μs, <100μs, <1ms, <10ms, <100ms, >100ms
for (int i = 0; i < 7; ++i)
    fmt::print("  bucket[{}]: {}\n", i, lat.buckets[i].load());
```

### Drop reasons

```cpp
for (int i = 0; i < static_cast<int>(gn::DropReason::_Count); ++i)
    if (snap.drops[i] > 0)
        fmt::print("  drop[{}]: {}\n", i, snap.drops[i]);
```

### C API

```c
gn_stats_t stats;
gn_core_get_stats(core, &stats);
printf("RX: %lu bytes\n", stats.rx_bytes);
printf("Dispatch latency avg: %lu ns\n", stats.dispatch_lat_avg);
```

---

*← [05 — Системные сообщения](05-system-messages.md) · [07 — Архитектура →](07-architecture.md)*
