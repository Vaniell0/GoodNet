# 06 — SignalBus

`include/signals.hpp`

## Назначение

`SignalBus` маршрутизирует расшифрованные пакеты от `ConnectionManager::dispatch_packet()` к хендлер-плагинам. Каждый хендлер подписывается на конкретные типы сообщений или на все сразу (wildcard). Доставка асинхронна — через отдельный `boost::asio::strand` для каждого хендлера.

---

## Signal\<Args...\>

Базовый thread-safe сигнал с snapshot-семантикой.

```cpp
template<typename... Args>
class Signal {
    mutable std::mutex mu_;
    boost::asio::strand<boost::asio::io_context::executor_type> strand_;
    std::vector<std::function<void(Args...)>> handlers_;
public:
    void connect(std::function<void(Args...)> h);
    void emit(Args... args);
    void clear();
    bool   empty() const;
    size_t size()  const;
};
```

### emit — snapshot под lock

```cpp
void emit(Args... args) {
    std::vector<std::function<void(Args...)>> snap;
    { std::lock_guard lk(mu_); snap = handlers_; }

    for (auto& h : snap)
        boost::asio::post(strand_,
            [h, args...]() mutable {
                try { h(args...); } catch (...) {}
            });
}
```

Snapshot берётся под lock, вызовы уходят в strand асинхронно. Отписка во время emit безопасна — текущая волна пакетов дойдёт до всех, кто был подписан в момент emit.

---

## Strand-изоляция

| Гарантия | Механизм |
|---|---|
| Хендлеры разных типов исполняются параллельно | У каждого свой strand |
| Пакеты одному хендлеру — строго FIFO | Strand = сериализованный executor |
| Нет гонок данных в коде хендлера | Дополнительные mutex не нужны |
| Исключение в одном хендлере не ломает другие | `catch (...)` в каждом `post` |

---

## Структура SignalBus

```
SignalBus
├── channels_[msg_type][handler_name] → HandlerPacketSignal
└── wildcards_[handler_name]          → HandlerPacketSignal

HandlerPacketSignal = Signal<
    string_view,              // имя хендлера
    shared_ptr<header_t>,     // расшифрованный заголовок
    const endpoint_t*,        // пир
    PacketData                // shared_ptr<vector<uint8_t>>
>
```

### Подписка

```cpp
// Конкретный тип:
bus_.subscribe(MSG_TYPE_CHAT, "chat_handler", callback);

// Wildcard (все типы, например Logger):
bus_.subscribe_wildcard("logger_handler", callback);
```

### emit

```cpp
// Вызывается ПОСЛЕ освобождения records_mu_:
void emit(uint32_t type, shared_ptr<header_t> hdr,
          const endpoint_t* ep, PacketData data)
{
    if (auto it = channels_.find(type); it != channels_.end())
        for (auto& [n, sig] : it->second)
            sig->emit(n, hdr, ep, data);

    for (auto& [n, sig] : wildcards_)
        sig->emit(n, hdr, ep, data);
}
```

`PacketData = shared_ptr<vector<uint8_t>>` — данные разделяются между всеми хендлерами без копирования.

---

*← [05 — ConnectionManager](05-connection-manager.md) · [07 — PluginManager →](07-plugin-manager.md)*
