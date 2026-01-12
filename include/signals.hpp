#pragma once
#include <span>
#include <vector>
#include <concepts>
#include <mutex>
#include "../sdk/types.h"

namespace gn {

// Концепт для типобезопасных сигналов
template<typename Func, typename... Args>
concept SignalHandler = std::invocable<Func, Args...>;

// Базовый класс для сигналов
template<typename... Args>
class Signal {
private:
    mutable std::mutex mutex_;  // <-- ДОБАВЬ mutable ЗДЕСЬ
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

// Реализация шаблонных методов
template<typename... Args>
template<typename Func>
requires SignalHandler<Func, Args...>
void Signal<Args...>::connect(Func&& handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    handlers_.emplace_back(std::forward<Func>(handler));
}

// Предопределенные типы сигналов
using PacketSignal = Signal<
    const header_t*,
    const endpoint_t*,
    std::span<const char>
>;

using ConnStateSignal = Signal<
    const char*,
    conn_state_t
>;

} // namespace gn
