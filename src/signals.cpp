#include "signals.hpp"
#include <boost/asio/strand.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/io_context.hpp>

namespace gn {

// --- 1. EventSignal (Control Plane - асинхронные события) ---

struct EventSignalBase::Impl {
    boost::asio::strand<boost::asio::io_context::executor_type> strand;
    std::atomic<size_t> pending_tasks{0}; // Счетчик задач
    const size_t MAX_PENDING = 10000;     // Жесткий лимит очереди

    explicit Impl(boost::asio::io_context& ioc) 
        : strand(boost::asio::make_strand(ioc.get_executor())) {}
};

EventSignalBase::EventSignalBase(boost::asio::io_context& ioc) 
    : impl_(std::make_unique<Impl>(ioc)) {}

EventSignalBase::~EventSignalBase() = default;

bool EventSignalBase::try_post_to_strand(std::function<void()> task) {
    // BACKPRESSURE: Защита от OOM
    if (impl_->pending_tasks.load(std::memory_order_relaxed) >= impl_->MAX_PENDING) {
        return false; // Дропаем событие (например, слишком много логов)
    }

    impl_->pending_tasks.fetch_add(1, std::memory_order_relaxed);
    
    boost::asio::post(impl_->strand, [this, t = std::move(task)]() {
        t();
        impl_->pending_tasks.fetch_sub(1, std::memory_order_relaxed);
    });
    
    return true;
}

// --- 2. PipelineSignal (Data Plane - горячий путь пакетов) ---

void PipelineSignal::connect(uint8_t priority, std::string_view name, HandlerPacketFn fn) {
    std::unique_lock lock(mu_);
    
    // RCU: Создаем полную копию данных для изменения
    auto next_handlers = std::make_shared<std::vector<Entry>>(*handlers_ptr_);
    
    // Удаляем старый хендлер с таким же именем (идемпотентность)
    next_handlers->erase(
        std::remove_if(next_handlers->begin(), next_handlers->end(),
                       [&](const Entry& e) { return e.name == name; }),
        next_handlers->end());

    next_handlers->push_back(Entry{ priority, std::string(name), std::move(fn) });

    // Сортируем: чем выше priority, тем раньше в векторе
    std::stable_sort(next_handlers->begin(), next_handlers->end(),
                     [](const Entry& a, const Entry& b) {
                         return a.priority > b.priority;
                     });

    // Атомарно подменяем указатель. Старая версия удалится сама, 
    // когда последний поток выйдет из emit().
    handlers_ptr_ = std::move(next_handlers);
}

void PipelineSignal::disconnect(std::string_view name) {
    std::unique_lock lock(mu_);
    auto next_handlers = std::make_shared<std::vector<Entry>>(*handlers_ptr_);
    
    next_handlers->erase(
        std::remove_if(next_handlers->begin(), next_handlers->end(),
                       [&](const Entry& e) { return e.name == name; }),
        next_handlers->end());

    handlers_ptr_ = std::move(next_handlers);
}

PipelineSignal::EmitResult PipelineSignal::emit(std::shared_ptr<header_t> hdr, 
                                               const endpoint_t* ep, 
                                               PacketData data) const {
    // ЧТЕНИЕ (Hot Path): Захватываем shared_ptr. 
    // shared_lock здесь нужен только для защиты самого указателя handlers_ptr_.
    std::shared_ptr<const std::vector<Entry>> snap;
    {
        std::shared_lock lock(mu_);
        snap = handlers_ptr_;
    }

    // Итерируемся по константной копии — это потокобезопасно без мьютексов!
    for (const auto& entry : *snap) {
        propagation_t r = PROPAGATION_CONTINUE;
        try {
            r = entry.fn(entry.name, hdr, ep, data);
        } catch (...) {
            // Ошибка в плагине не должна вешать ядро
            continue; 
        }

        if (r == PROPAGATION_CONSUMED) return { PROPAGATION_CONSUMED, entry.name };
        if (r == PROPAGATION_REJECT)   return { PROPAGATION_REJECT, entry.name };
    }
    return { PROPAGATION_CONTINUE, "" };
}

// --- 3. SignalBus (Управление каналами) ---

SignalBus::SignalBus(boost::asio::io_context& ioc) 
    : on_log(ioc), 
      on_connection_state(ioc) {}

void SignalBus::subscribe(uint32_t msg_type, std::string_view name, HandlerPacketFn cb, uint8_t prio) {
    std::unique_lock lock(mu_);
    auto& sig = channels_[msg_type];
    if (!sig) sig = std::make_unique<PipelineSignal>();
    sig->connect(prio, name, std::move(cb));
}

void SignalBus::subscribe_wildcard(std::string_view name, HandlerPacketFn cb, uint8_t prio) {
    wildcards_.connect(prio, name, std::move(cb));
}

PipelineSignal::EmitResult SignalBus::dispatch_packet(uint32_t msg_type, 
                                                    std::shared_ptr<header_t> hdr, 
                                                    const endpoint_t* ep, 
                                                    PacketData data) {
    // 1. Сначала специфичный канал для этого типа сообщения
    PipelineSignal* target = nullptr;
    {
        std::shared_lock lock(mu_);
        if (auto it = channels_.find(msg_type); it != channels_.end()) {
            target = it->second.get();
        }
    }

    if (target) {
        auto res = target->emit(hdr, ep, data);
        if (res.result != PROPAGATION_CONTINUE) return res;
    }

    // 2. Если никто не поглотил пакет, отдаем его Wildcard-подписчикам
    return wildcards_.emit(hdr, ep, data);
}

} // namespace gn
