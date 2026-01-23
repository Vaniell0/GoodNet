#pragma once
#include <span>
#include <vector>
#include <concepts>
#include <mutex>
#include "../sdk/types.h"

namespace gn {

template<typename Func, typename... Args>
concept SignalHandler = std::invocable<Func, Args...>;

template<typename... Args>
class Signal {
private:
    mutable std::mutex mutex_;
    boost::asio::io_context& io_context_;
    boost::asio::strand<boost::asio::io_context::executor_type> strand_;
    std::vector<std::function<void(Args...)>> handlers_;

public:
    explicit Signal(boost::asio::io_context& io_context);
    
    // Подписка на сигнал
    template<typename Func>
    requires SignalHandler<Func, Args...>
    void connect(Func&& handler);
    
    // Асинхронная эмиссия сигнала
    void emit(Args... args);
    
    void disconnect_all();
    [[nodiscard]] size_t size() const;
};

template<typename... Args>
template<typename Func>
requires SignalHandler<Func, Args...>
void Signal<Args...>::connect(Func&& handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    handlers_.emplace_back(std::forward<Func>(handler));
}

using PacketSignal = Signal<
    const header_t*,
    const endpoint_t*,
    std::span<const char>
>;

using ConnStateSignal = Signal<
    const char*,
    conn_state_t
>;

extern std::shared_ptr<PacketSignal> packet_signal;
extern std::shared_ptr<ConnStateSignal> conn_state_signal;

} // namespace gn