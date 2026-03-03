#pragma once
#include <span>
#include <vector>
#include <concepts>
#include <functional>
#include <boost/asio.hpp>
#include "../sdk/types.h"

namespace gn {

// Используем shared_ptr для безопасности данных
using PacketData = std::shared_ptr<std::vector<char>>;

template<typename Func, typename... Args>
concept SignalHandler = std::invocable<Func, Args...>;

template<typename... Args>
class Signal {
private:
    mutable std::mutex mutex_;
    boost::asio::strand<boost::asio::io_context::executor_type> strand_;
    std::vector<std::function<void(Args...)>> handlers_;

public:
    explicit Signal(boost::asio::io_context& ioc)
        : strand_(boost::asio::make_strand(ioc.get_executor())) {}
    
    template<typename Func>
    requires SignalHandler<Func, Args...>
    void connect(Func&& h) {
        std::lock_guard lock(mutex_);
        handlers_.emplace_back(std::forward<Func>(h));
    }
    
    void emit(Args... args) {
        std::vector<std::function<void(Args...)>> targets;
        {
            std::lock_guard lock(mutex_);
            targets = handlers_;
        }

        for (auto& h : targets) {
            // Захватываем shared_ptr по значению в лямбду, увеличивая ref_count
            boost::asio::post(strand_, [h, args...]() mutable {
                try { h(args...); } catch (...) {}
            });
        }
    }
    
    void clear() {
        std::lock_guard lock(mutex_);
        handlers_.clear();
    }
};

// Финальные типы сигналов с shared_ptr
using PacketSignal = Signal<
    std::shared_ptr<header_t>, 
    const endpoint_t*, 
    PacketData
>;

}
