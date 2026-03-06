#pragma once

/// @file include/signals.hpp
/// @brief Async signal bus with per-channel strands.
///
/// Tree: bus[msg_type][handler_name] → HandlerPacketSignal
/// Wildcard subscribers receive all msg_types.
/// Each channel runs on a dedicated strand — handlers never block each other.

#include <string_view>
#include <vector>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include "../sdk/types.h"

namespace boost::asio { class io_context; }

namespace gn {

using PacketData = std::shared_ptr<std::vector<uint8_t>>;

class SignalBase {
public:
    explicit SignalBase(boost::asio::io_context& ioc);
    virtual ~SignalBase();

protected:
    void post_to_strand(std::function<void()> task);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_; // PIMPL для скрытия boost::asio::strand
};

/// @brief Thread-safe signal with strand-serialized dispatch.
template<typename... Args>
class Signal : public SignalBase {
public:
    using Handler = std::function<void(Args...)>;

    explicit Signal(boost::asio::io_context& ioc) : SignalBase(ioc) {}

    void connect(Handler h) {
        std::lock_guard lock(mu_);
        handlers_.push_back(std::move(h));
    }

    void emit(Args... args) {
        std::vector<Handler> snap;
        {
            std::lock_guard lock(mu_);
            snap = handlers_;
        }
        for (auto& h : snap) {
            // Передаем аргументы по значению в лямбду
            post_to_strand([h, args...]() mutable {
                try { h(args...); } catch (...) {}
            });
        }
    }

    void clear() { std::lock_guard lock(mu_); handlers_.clear(); }

private:
    mutable std::mutex mu_;
    std::vector<Handler> handlers_;
};

/// @brief Signal type for packet handlers: (handler_name, header, endpoint, payload).
using HandlerPacketSignal = Signal<
    std::string_view,
    std::shared_ptr<header_t>,
    const endpoint_t*,
    PacketData
>;

/// @brief Multi-channel signal bus keyed by msg_type and handler name.
class SignalBus {
public:
    explicit SignalBus(boost::asio::io_context& ioc);
    ~SignalBus();

    void subscribe(uint32_t msg_type, std::string_view name, HandlerPacketSignal::Handler cb);
    void subscribe_wildcard(std::string_view name, HandlerPacketSignal::Handler cb);
    
    void emit(uint32_t msg_type, std::shared_ptr<header_t> hdr, const endpoint_t* ep, PacketData data);

    void clear();

    size_t subscriber_count(uint32_t t) const {
        std::shared_lock lk(mu_);
        auto it = channels_.find(t);
        return (it != channels_.end()) ? it->second.size() : 0;
    }

    size_t wildcard_count() const { 
        std::shared_lock lk(mu_); 
        return wildcards_.size(); 
    }

private:
    boost::asio::io_context& ioc_;
    mutable std::shared_mutex mu_;
    
    using SignalMap = std::unordered_map<std::string, std::unique_ptr<HandlerPacketSignal>>;
    std::unordered_map<uint32_t, SignalMap> channels_;
    SignalMap wildcards_;
};

} // namespace gn
