#include "signals.hpp"
#include <boost/asio/strand.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/io_context.hpp>

namespace gn {

// Реализация PIMPL для SignalBase
struct SignalBase::Impl {
    boost::asio::strand<boost::asio::io_context::executor_type> strand;
    explicit Impl(boost::asio::io_context& ioc) 
        : strand(boost::asio::make_strand(ioc.get_executor())) {}
};

SignalBase::SignalBase(boost::asio::io_context& ioc) 
    : impl_(std::make_unique<Impl>(ioc)) {}

SignalBase::~SignalBase() = default;

void SignalBase::post_to_strand(std::function<void()> task) {
    boost::asio::post(impl_->strand, std::move(task));
}

// --- SignalBus Implementation ---

SignalBus::SignalBus(boost::asio::io_context& ioc) : ioc_(ioc) {}
SignalBus::~SignalBus() = default;

void SignalBus::subscribe(uint32_t msg_type, std::string_view name, HandlerPacketSignal::Handler cb) {
    std::string n(name);
    {
        std::unique_lock lk(mu_);
        if (channels_[msg_type].find(n) == channels_[msg_type].end()) {
            channels_[msg_type][n] = std::make_unique<HandlerPacketSignal>(ioc_);
        }
    }
    std::shared_lock lk(mu_);
    channels_.at(msg_type).at(n)->connect(std::move(cb));
}

void SignalBus::subscribe_wildcard(std::string_view name, HandlerPacketSignal::Handler cb) {
    std::string n(name);
    {
        std::unique_lock lk(mu_);
        if (wildcards_.find(n) == wildcards_.end()) {
            wildcards_[n] = std::make_unique<HandlerPacketSignal>(ioc_);
        }
    }
    std::shared_lock lk(mu_);
    wildcards_.at(n)->connect(std::move(cb));
}

void SignalBus::emit(uint32_t msg_type, std::shared_ptr<header_t> hdr, const endpoint_t* ep, PacketData data) {
    std::shared_lock lk(mu_);
    
    // Эмит в конкретный канал
    if (auto it = channels_.find(msg_type); it != channels_.end()) {
        for (auto& [name, sig] : it->second) {
            sig->emit(name, hdr, ep, data);
        }
    }
    
    // Эмит wildcard подписчикам
    for (auto& [name, sig] : wildcards_) {
        sig->emit(name, hdr, ep, data);
    }
}

void SignalBus::clear() {
    std::unique_lock lk(mu_);
    channels_.clear();
    wildcards_.clear();
}

} // namespace gn
