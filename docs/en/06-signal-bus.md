# 06 — SignalBus

`include/signals.hpp`

---

## Purpose

`SignalBus` routes decrypted packets from `ConnectionManager::dispatch_packet()` to handler plugins. Each handler subscribes to specific message types or all of them (wildcard). Delivery is asynchronous — through a dedicated `boost::asio::strand` per handler.

---

## Signal\<Args...\>

Thread-safe signal with snapshot semantics.

```cpp
template<typename... Args>
class Signal {
    mutable std::mutex mu_;
    boost::asio::strand<...> strand_;
    std::vector<std::function<void(Args...)>> handlers_;
public:
    void connect(std::function<void(Args...)> h);
    void emit(Args... args);
    void clear();
    bool   empty() const;
    size_t size()  const;
};
```

### How emit works

```cpp
void emit(Args... args) {
    std::vector<std::function<void(Args...)>> snap;
    { std::lock_guard lk(mu_); snap = handlers_; }  // snapshot under lock

    for (auto& h : snap)
        boost::asio::post(strand_,
            [h, args...]() mutable {
                try { h(args...); } catch (...) {}
            });
}
```

---

## Strand Isolation

| Guarantee | Mechanism |
|---|---|
| Different handlers execute in parallel | Each has its own strand |
| Packets to one handler are strictly FIFO | Strand = serialized executor |
| No data races in handler code | No extra mutexes needed |
| Exception in one handler doesn't break others | `catch(...)` in each `post` |

---

## SignalBus Structure

```
SignalBus
├── channels_[msg_type][handler_name] → HandlerPacketSignal
└── wildcards_[handler_name]          → HandlerPacketSignal

HandlerPacketSignal = Signal<
    string_view,              // handler name
    shared_ptr<header_t>,     // decrypted header
    const endpoint_t*,        // remote peer
    PacketData                // shared_ptr<vector<uint8_t>>
>
```

### Subscription

```cpp
// Specific type:
bus_.subscribe(MSG_TYPE_CHAT, "chat_handler", callback);

// Wildcard (all types, e.g. Logger):
bus_.subscribe_wildcard("logger_handler", callback);
```

`PacketData = shared_ptr<vector<uint8_t>>` — data is shared across all handlers without copying.

---

*← [05 — ConnectionManager](05-connection-manager.md) · [07 — PluginManager →](07-plugin-manager.md)*
