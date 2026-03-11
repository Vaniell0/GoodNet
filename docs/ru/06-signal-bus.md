# 06 — SignalBus

`include/signals.hpp`

## Назначение

`SignalBus` маршрутизирует расшифрованные пакеты от `ConnectionManager::dispatch_packet()` к хендлер-плагинам. Реализует **цепочку ответственности (chain-of-responsibility)**: хендлеры вызываются синхронно в порядке убывания приоритета, пока один из них не вернёт `PROPAGATION_CONSUMED` или `PROPAGATION_REJECT`.

---

## Два типа сигналов

### PipelineSignal — данные (горячий путь)

Синхронный, lock-free на пути чтения. Используется для пакетного трафика.

```cpp
class PipelineSignal {
public:
    void connect(uint8_t priority, std::string_view name, HandlerPacketFn fn);
    void disconnect(std::string_view name);

    struct EmitResult {
        propagation_t result = PROPAGATION_CONTINUE;
        std::string   consumed_by; // имя хендлера, поглотившего пакет
    };
    EmitResult emit(shared_ptr<header_t>, const endpoint_t*, PacketData) const;
};
```

**RCU-семантика:** `connect()` / `disconnect()` атомарно подменяют `shared_ptr<const vector<Entry>>`. Читающие потоки `emit()` захватывают snapshot указателя под `shared_lock` (одна атомарная операция) и итерируются по нему без блокировок.

### EventSignal — управление (контрольный путь)

Асинхронный, через `boost::asio::strand`. Используется для событий: логов, изменений состояния соединений.

```cpp
template<typename... Args>
class EventSignal : public EventSignalBase {
public:
    void connect(Handler h);
    void emit(Args... args);   // snapshot + post в strand
};
```

---

## propagation_t

Определён в `sdk/types.h`. Управляет цепочкой вызовов хендлеров:

```c
typedef enum {
    PROPAGATION_CONTINUE = 0, // передать следующему по приоритету
    PROPAGATION_CONSUMED = 1, // остановить цепочку; пинит session affinity
    PROPAGATION_REJECT   = 2  // дропнуть пакет молча
} propagation_t;
```

Значение берётся из `handler_t::on_message_result()` — опциональный коллбэк. `NULL` трактуется как `PROPAGATION_CONTINUE`.

---

## Порядок диспетчеризации

```
dispatch_packet(msg_type, hdr, ep, data)
        │
        ├─ 1. channels_[msg_type] — специфичный канал
        │       handlers отсортированы по priority (255 = первый)
        │       for each handler:
        │           handler.handle_message(...)
        │           r = handler.on_message_result(...)
        │           if r == CONSUMED → return {CONSUMED, handler.name}
        │           if r == REJECT   → return {REJECT,   handler.name}
        │           // CONTINUE → следующий хендлер
        │
        └─ 2. wildcards_ — если channels_[type] никто не CONSUMED/REJECTED
                same iteration logic
```

**Session affinity:** при первом `CONSUMED` `conn_id` привязывается к имени хендлера (`affinity_plugin`). Последующие пакеты того же соединения по-прежнему проходят через полную цепочку — affinity используется только как диагностика и потенциальный hint для будущей оптимизации.

---

## Структура SignalBus

```cpp
class SignalBus {
public:
    // Подписка на конкретный тип пакета
    void subscribe(uint32_t msg_type, std::string_view name,
                   HandlerPacketFn cb, uint8_t prio = 128);

    // Wildcard — все типы (например Logger)
    void subscribe_wildcard(std::string_view name,
                            HandlerPacketFn cb, uint8_t prio = 128);

    // Диспетчеризация — вызывается из dispatch_packet() после unlock records_mu_
    PipelineSignal::EmitResult dispatch_packet(
        uint32_t msg_type,
        shared_ptr<header_t> hdr,
        const endpoint_t* ep,
        PacketData data);

    // Контрольные события
    EventSignal<std::string>        on_log;
    EventSignal<uint32_t, bool>     on_connection_state;
};
```

`PacketData = shared_ptr<vector<uint8_t>>` — данные разделяются между всеми хендлерами без копирования.

---

## Backpressure для EventSignal

`EventSignalBase` реализует атомарный счётчик задач с жёстким лимитом очереди. При `pending_tasks >= MAX_PENDING` новое событие дропается — это защищает Asio-очередь от OOM при логовых штормах:

```cpp
bool try_post_to_strand(std::function<void()> task) {
    if (pending_tasks_.load() >= MAX_PENDING) return false; // drop
    pending_tasks_.fetch_add(1);
    boost::asio::post(strand_, [this, t = move(task)] {
        t();
        pending_tasks_.fetch_sub(1);
    });
    return true;
}
```

---

## Thread safety

| Операция | Гарантия |
|---|---|
| `PipelineSignal::emit()` | Lock-free на горячем пути (snapshot read) |
| `PipelineSignal::connect()` / `disconnect()` | `unique_lock` только при изменении |
| `EventSignal::emit()` | `mutex` для snapshot, затем async post |
| `SignalBus::subscribe*()` | `unique_lock` при добавлении канала |
| `SignalBus::dispatch_packet()` | `shared_lock` для поиска канала, затем lock-free emit |

---

*← [05 — ConnectionManager](05-connection-manager.md) · [07 — PluginManager →](07-plugin-manager.md)*