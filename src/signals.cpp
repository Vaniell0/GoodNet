#include <boost/asio.hpp>
#include "signals.hpp"
#include "logger.hpp"

namespace gn {

template<typename... Args>
Signal<Args...>::Signal(boost::asio::io_context& io_context)
    : io_context_(io_context),
      strand_(boost::asio::make_strand(io_context.get_executor())) {}

template<typename... Args>
void Signal<Args...>::emit(Args... args) {
    std::vector<std::function<void(Args...)>> handlers_copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        handlers_copy = handlers_;
    }
    
    // Используем нормальное логирование
    LOG_DEBUG("Signal emitting to {} handlers", handlers_copy.size());
    
    for (auto& handler : handlers_copy) {
        boost::asio::post(strand_, [handler, args...]() mutable {
            try {
                handler(args...);
            } catch (const std::exception& e) {
                LOG_ERROR("Signal handler error: {}", e.what());
            }
        });
    }
}

template<typename... Args>
void Signal<Args...>::disconnect_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    handlers_.clear();
    LOG_DEBUG("All signal handlers disconnected");
}

template<typename... Args>
size_t Signal<Args...>::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return handlers_.size();
}

// Явные инстанцирования для часто используемых типов
template class Signal<const header_t*, const endpoint_t*, std::span<const char>>;
template class Signal<const char*, conn_state_t>;

} // namespace gn
