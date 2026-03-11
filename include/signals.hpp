#pragma once

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

// --- 1. PIPELINE (Для пакетов, синхронно, быстро) ---

using HandlerPacketFn = std::function<
    propagation_t(std::string_view handler_name,
                  std::shared_ptr<header_t> hdr,
                  const endpoint_t* ep,
                  PacketData data)>;

class PipelineSignal {
public:
    explicit PipelineSignal() 
        : handlers_ptr_(std::make_shared<const std::vector<Entry>>()) {}

    void connect(uint8_t priority, std::string_view name, HandlerPacketFn fn);
    void disconnect(std::string_view name);

    struct EmitResult {
        propagation_t result = PROPAGATION_CONTINUE;
        std::string consumed_by;
    };

    EmitResult emit(std::shared_ptr<header_t> hdr, const endpoint_t* ep, PacketData data) const;

private:
    struct Entry {
        uint8_t priority;
        std::string name;
        HandlerPacketFn fn;
    };
    mutable std::shared_mutex mu_;
    std::shared_ptr<const std::vector<Entry>> handlers_ptr_;
};

// --- 2. EVENTS (Для уведомлений, асинхронно, через strand) ---

class EventSignalBase {
public:
    explicit EventSignalBase(boost::asio::io_context& ioc);
    virtual ~EventSignalBase();
protected:
    bool try_post_to_strand(std::function<void()> task);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

template<typename... Args>
class EventSignal : public EventSignalBase {
public:
    using Handler = std::function<void(Args...)>;
    explicit EventSignal(boost::asio::io_context& ioc) : EventSignalBase(ioc) {}

    void connect(Handler h) {
        std::lock_guard lock(mu_);
        handlers_.push_back(std::move(h));
    }

    void emit(Args... args) {
        std::unique_lock lock(mu_);
        auto snap = handlers_;
        lock.unlock();

        for (auto& h : snap) {
            try_post_to_strand([h, args...]() mutable {
                try { h(args...); } catch (...) {}
            });
        }
    }
private:
    mutable std::mutex mu_;
    std::vector<Handler> handlers_;
};

// --- 3. BUS (Управляющий центр) ---

class SignalBus {
public:
    explicit SignalBus(boost::asio::io_context& ioc);

    // Подписка на пакеты
    void subscribe(uint32_t msg_type, std::string_view name, HandlerPacketFn cb, uint8_t prio = 128);
    void subscribe_wildcard(std::string_view name, HandlerPacketFn cb, uint8_t prio = 128);

    // Диспетчеризация пакета
    PipelineSignal::EmitResult dispatch_packet(uint32_t msg_type, std::shared_ptr<header_t> hdr, 
                                              const endpoint_t* ep, PacketData data);

    // Сигналы событий (публичные поля для прямого доступа)
    EventSignal<std::string> on_log;
    EventSignal<uint32_t /*conn_id*/, bool /*connected*/> on_connection_state;

private:
    mutable std::shared_mutex mu_;
    std::unordered_map<uint32_t, std::unique_ptr<PipelineSignal>> channels_;
    PipelineSignal wildcards_;
};

} // namespace gn
