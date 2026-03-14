/// @file src/signals.cpp

#include "signals.hpp"

#include <boost/asio.hpp>
#include <algorithm>

namespace gn {

namespace asio = boost::asio;

// ── EventSignalBase ───────────────────────────────────────────────────────────

struct EventSignalBase::Impl {
    asio::strand<asio::io_context::executor_type> strand;
    explicit Impl(asio::io_context& ioc) : strand(asio::make_strand(ioc)) {}
};

EventSignalBase::EventSignalBase(asio::io_context& ioc)
    : impl_(std::make_shared<Impl>(ioc)) {}

EventSignalBase::~EventSignalBase() = default;

bool EventSignalBase::try_post_to_strand(std::function<void()> task) {
    auto impl = impl_;
    asio::post(impl->strand, [impl, t = std::move(task)]() mutable { t(); });
    return true;
}

// ── PipelineSignal ────────────────────────────────────────────────────────────

void PipelineSignal::connect(uint8_t priority, std::string_view name, HandlerPacketFn fn) {
    std::lock_guard lock(write_mu_);
    auto old = handlers_ptr_.load(std::memory_order_acquire);
    auto vec  = std::make_shared<std::vector<Entry>>(*old);
    vec->push_back({priority, std::string(name), std::move(fn)});
    std::stable_sort(vec->begin(), vec->end(),
        [](const Entry& a, const Entry& b) { return a.priority > b.priority; });
    handlers_ptr_.store(std::move(vec), std::memory_order_release);
}

void PipelineSignal::disconnect(std::string_view name) {
    std::lock_guard lock(write_mu_);
    auto old = handlers_ptr_.load(std::memory_order_acquire);
    auto vec  = std::make_shared<std::vector<Entry>>();
    for (auto& e : *old)
        if (e.name != name) vec->push_back(e);
    handlers_ptr_.store(std::move(vec), std::memory_order_release);
}

PipelineSignal::EmitResult PipelineSignal::emit(
        std::shared_ptr<header_t> hdr,
        const endpoint_t*         ep,
        PacketData                data) const {
    auto handlers = handlers_ptr_.load(std::memory_order_acquire);
    for (auto& e : *handlers) {
        auto r = e.fn(e.name, hdr, ep, data);
        if (r == PROPAGATION_CONSUMED) return {PROPAGATION_CONSUMED, e.name};
        if (r == PROPAGATION_REJECT)   return {PROPAGATION_REJECT,   e.name};
    }
    return {};
}

// ── SignalBus ─────────────────────────────────────────────────────────────────

SignalBus::SignalBus(asio::io_context& ioc)
    : on_stat(ioc), on_log(ioc), on_conn_state(ioc) {}

uint64_t SignalBus::subscribe(uint32_t msg_type, std::string_view name,
                               HandlerPacketFn cb, uint8_t prio) {
    const uint64_t id = next_sub_id_.fetch_add(1, std::memory_order_relaxed);
    {
        std::unique_lock lock(sub_mu_);
        sub_map_[id] = {msg_type, std::string(name)};
    }
    {
        std::unique_lock lock(mu_);
        auto& ch = channels_[msg_type];
        if (!ch) ch = std::make_unique<PipelineSignal>();
        ch->connect(prio, name, std::move(cb));
    }
    return id;
}

void SignalBus::subscribe_wildcard(std::string_view name,
                                    HandlerPacketFn cb, uint8_t prio) {
    wildcards_.connect(prio, name, std::move(cb));
}

void SignalBus::unsubscribe(uint64_t sub_id) {
    std::unique_lock lock(sub_mu_);
    auto it = sub_map_.find(sub_id);
    if (it == sub_map_.end()) return;
    const auto& [msg_type, name] = it->second;

    std::unique_lock chlk(mu_);
    if (auto cit = channels_.find(msg_type); cit != channels_.end())
        cit->second->disconnect(name);
    sub_map_.erase(it);
}

PipelineSignal::EmitResult SignalBus::dispatch_packet(
        uint32_t                  msg_type,
        std::shared_ptr<header_t> hdr,
        const endpoint_t*         ep,
        PacketData                data) {
    {
        std::shared_lock lock(mu_);
        auto it = channels_.find(msg_type);
        if (it != channels_.end()) {
            auto r = it->second->emit(hdr, ep, data);
            if (r.result != PROPAGATION_CONTINUE) return r;
        }
    }
    return wildcards_.emit(hdr, ep, data);
}

// ── Stats ─────────────────────────────────────────────────────────────────────

void SignalBus::emit_stat(StatsEvent ev) noexcept {
    using K = StatsEvent::Kind;
    auto& a = accum_;
    switch (ev.kind) {
        case K::RxBytes:      a.rx_bytes    .fetch_add(ev.value, std::memory_order_relaxed); break;
        case K::TxBytes:      a.tx_bytes    .fetch_add(ev.value, std::memory_order_relaxed); break;
        case K::RxPacket:     a.rx_packets  .fetch_add(1,        std::memory_order_relaxed); break;
        case K::TxPacket:     a.tx_packets  .fetch_add(ev.value, std::memory_order_relaxed); break;
        case K::AuthOk:       a.auth_ok     .fetch_add(1,        std::memory_order_relaxed); break;
        case K::AuthFail:     a.auth_fail   .fetch_add(1,        std::memory_order_relaxed); break;
        case K::DecryptFail:  a.decrypt_fail.fetch_add(1,        std::memory_order_relaxed); break;
        case K::Backpressure: a.backpressure.fetch_add(1,        std::memory_order_relaxed); break;
        case K::Consumed:     a.consumed    .fetch_add(1,        std::memory_order_relaxed); break;
        case K::Rejected:     a.rejected    .fetch_add(1,        std::memory_order_relaxed); break;
        case K::Connect:
            a.connections.fetch_add(1, std::memory_order_relaxed);
            a.total_conn .fetch_add(1, std::memory_order_relaxed);
            break;
        case K::Disconnect:
            a.connections.fetch_sub(1, std::memory_order_relaxed);
            a.total_disc .fetch_add(1, std::memory_order_relaxed);
            break;
        case K::Drop:
            a.drops[static_cast<size_t>(ev.drop_reason)]
                .fetch_add(1, std::memory_order_relaxed);
            break;
        case K::DispatchLatencyNs:
            a.dispatch_lat.record(ev.value);
            break;
    }
    on_stat.emit(ev);
}

void SignalBus::emit_drop(conn_id_t id, DropReason why) noexcept {
    StatsEvent ev;
    ev.kind        = StatsEvent::Kind::Drop;
    ev.value       = 1;
    ev.conn_id     = id;
    ev.drop_reason = why;
    emit_stat(ev);
}

void SignalBus::emit_latency(conn_id_t id, uint64_t ns) noexcept {
    StatsEvent ev;
    ev.kind    = StatsEvent::Kind::DispatchLatencyNs;
    ev.value   = ns;
    ev.conn_id = id;
    emit_stat(ev);
}

StatsSnapshot SignalBus::stats_snapshot() const noexcept {
    auto& a = accum_;
    StatsSnapshot s;
    s.rx_bytes     = a.rx_bytes    .load(std::memory_order_relaxed);
    s.tx_bytes     = a.tx_bytes    .load(std::memory_order_relaxed);
    s.rx_packets   = a.rx_packets  .load(std::memory_order_relaxed);
    s.tx_packets   = a.tx_packets  .load(std::memory_order_relaxed);
    s.auth_ok      = a.auth_ok     .load(std::memory_order_relaxed);
    s.auth_fail    = a.auth_fail   .load(std::memory_order_relaxed);
    s.decrypt_fail = a.decrypt_fail.load(std::memory_order_relaxed);
    s.backpressure = a.backpressure.load(std::memory_order_relaxed);
    s.consumed     = a.consumed    .load(std::memory_order_relaxed);
    s.rejected     = a.rejected    .load(std::memory_order_relaxed);
    s.connections  = a.connections .load(std::memory_order_relaxed);
    s.total_conn   = a.total_conn  .load(std::memory_order_relaxed);
    s.total_disc   = a.total_disc  .load(std::memory_order_relaxed);
    for (size_t i = 0; i < 12; ++i)
        s.drops[i] = a.drops[i].load(std::memory_order_relaxed);
    // Copy histogram (non-atomic read — approximate, good enough for telemetry)
    for (int i = 0; i < 7; ++i)
        s.dispatch_latency.buckets[i].store(
            a.dispatch_lat.buckets[i].load(std::memory_order_relaxed));
    s.dispatch_latency.total_ns.store(
        a.dispatch_lat.total_ns.load(std::memory_order_relaxed));
    s.dispatch_latency.count.store(
        a.dispatch_lat.count.load(std::memory_order_relaxed));
    return s;
}

} // namespace gn
