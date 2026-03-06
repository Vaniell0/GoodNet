#pragma once

/// @file include/signals.hpp
/// @brief Async signal bus with per-channel strands.
///
/// Tree: bus[msg_type][handler_name] → HandlerPacketSignal
/// Wildcard subscribers receive all msg_types.
/// Each channel runs on a dedicated strand — handlers never block each other.

#include <string>
#include <string_view>
#include <vector>
#include <functional>
#include <concepts>
#include <mutex>
#include <shared_mutex>
#include <memory>
#include <unordered_map>
#include <boost/asio.hpp>
#include "../sdk/types.h"

namespace gn {

using PacketData = std::shared_ptr<std::vector<uint8_t>>;

template<typename Func, typename... Args>
concept SignalHandler = std::invocable<Func, Args...>;

/// @brief Thread-safe signal with strand-serialized dispatch.
template<typename... Args>
class Signal {
public:
    explicit Signal(boost::asio::io_context& ioc)
        : strand_(boost::asio::make_strand(ioc.get_executor())) {}

    template<typename Func>
    requires SignalHandler<Func, Args...>
    void connect(Func&& h) {
        std::lock_guard lock(mu_);
        handlers_.emplace_back(std::forward<Func>(h));
    }

    void emit(Args... args) {
        std::vector<std::function<void(Args...)>> snap;
        { std::lock_guard lock(mu_); snap = handlers_; }
        for (auto& h : snap)
            boost::asio::post(strand_, [h, args...]() mutable {
                try { h(args...); } catch (...) {}
            });
    }

    void   clear()  { std::lock_guard lock(mu_); handlers_.clear(); }
    bool   empty()  const { std::lock_guard lock(mu_); return handlers_.empty(); }
    size_t size()   const { std::lock_guard lock(mu_); return handlers_.size(); }

private:
    mutable std::mutex mu_;
    boost::asio::strand<boost::asio::io_context::executor_type> strand_;
    std::vector<std::function<void(Args...)>> handlers_;
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
    explicit SignalBus(boost::asio::io_context& ioc) : ioc_(ioc) {}

    /// @brief Subscribe handler to a specific msg_type.
    template<typename Func>
    void subscribe(uint32_t msg_type, std::string_view name, Func&& cb) {
        const std::string n(name);
        {
            std::unique_lock lk(mu_);
            if (!channels_[msg_type].count(n))
                channels_[msg_type].emplace(n, std::make_unique<HandlerPacketSignal>(ioc_));
        }
        std::shared_lock lk(mu_);
        channels_.at(msg_type).at(n)->connect(std::forward<Func>(cb));
    }

    /// @brief Subscribe handler to all msg_types (wildcard).
    template<typename Func>
    void subscribe_wildcard(std::string_view name, Func&& cb) {
        const std::string n(name);
        {
            std::unique_lock lk(mu_);
            if (!wildcards_.count(n))
                wildcards_.emplace(n, std::make_unique<HandlerPacketSignal>(ioc_));
        }
        std::shared_lock lk(mu_);
        wildcards_.at(n)->connect(std::forward<Func>(cb));
    }

    /// @brief Dispatch packet to all subscribers of msg_type and all wildcards.
    void emit(uint32_t                  msg_type,
              std::shared_ptr<header_t> hdr,
              const endpoint_t*         ep,
              PacketData                data)
    {
        {
            std::shared_lock lk(mu_);
            if (auto it = channels_.find(msg_type); it != channels_.end())
                for (auto& [n, sig] : it->second)
                    sig->emit(n, hdr, ep, data);
        }
        {
            std::shared_lock lk(mu_);
            for (auto& [n, sig] : wildcards_)
                sig->emit(n, hdr, ep, data);
        }
    }

    size_t subscriber_count(uint32_t t) const {
        std::shared_lock lk(mu_);
        if (auto it = channels_.find(t); it != channels_.end()) return it->second.size();
        return 0;
    }

    size_t wildcard_count() const { std::shared_lock lk(mu_); return wildcards_.size(); }
    void   clear()          { std::unique_lock lk(mu_); channels_.clear(); wildcards_.clear(); }

private:
    boost::asio::io_context& ioc_;
    mutable std::shared_mutex mu_;
    std::unordered_map<uint32_t,
        std::unordered_map<std::string, std::unique_ptr<HandlerPacketSignal>>> channels_;
    std::unordered_map<std::string, std::unique_ptr<HandlerPacketSignal>> wildcards_;
};

} // namespace gn
