/// @file core/cm/registration.cpp
/// Handler and connector registration for ConnectionManager.

#include "impl.hpp"
#include "logger.hpp"
#include "signals.hpp"

namespace gn {

// =============================================================================
// Connector registration
// =============================================================================

void ConnectionManager::Impl::register_connector(const std::string& scheme,
                                                   connector_ops_t* ops) {
    std::unique_lock lock(connectors_mu_);
    connectors_[scheme] = ops;
    LOG_DEBUG("Connector '{}' registered", scheme);
}

// =============================================================================
// Handler registration
// =============================================================================

void ConnectionManager::Impl::register_handler(handler_t* h) {
    register_handler_internal(h, HandlerSource::Plugin);
}

void ConnectionManager::Impl::register_handler_from_connector(handler_t* h) {
    register_handler_internal(h, HandlerSource::Connector);
}

bool ConnectionManager::Impl::is_connector_blocked_type(uint32_t msg_type) {
    // Типы 0-9: ядро (SYSTEM, NOISE_INIT/RESP/FIN, HEARTBEAT, резерв)
    if (msg_type <= 9) return true;
    // Системный диапазон 0x0100-0x0FFF: DHT, Health, RPC, Routing, TUN
    if (msg_type >= MSG_TYPE_SYS_BASE && msg_type <= MSG_TYPE_SYS_MAX) return true;
    return false;
}

void ConnectionManager::Impl::register_handler_internal(handler_t* h,
                                                          HandlerSource source) {
    if (!h || !h->name) return;
    const std::string name(h->name);
    const uint8_t priority = (h->info && h->info->priority) ? h->info->priority : 128u;
    LOG_TRACE("register_handler '{}': {} types, prio={}, source={}",
              name, h->num_supported_types, priority,
              source == HandlerSource::Connector ? "connector" : "plugin");

    // Коннекторам запрещена wildcard-подписка
    if (source == HandlerSource::Connector && !h->num_supported_types) {
        LOG_WARN("register_handler '{}': wildcard subscription blocked "
                 "for connector-registered handler", name);
        return;
    }

    auto make_cb = [h, name](std::string_view,
                              std::shared_ptr<header_t> hdr,
                              const endpoint_t*          ep,
                              PacketData                 data) -> propagation_t {
        if (h->handle_message)
            h->handle_message(h->user_data, hdr.get(), ep,
                              data->data(), data->size());
        if (h->on_message_result)
            return h->on_message_result(h->user_data, hdr.get(),
                                         hdr->payload_type);
        return PROPAGATION_CONTINUE;
    };

    HandlerEntry entry;
    entry.name    = name;
    entry.handler = h;
    entry.source  = source;

    if (!h->num_supported_types) {
        bus_.subscribe_wildcard(name, make_cb, priority);
        LOG_INFO("Handler '{}' registered (wildcard, prio={})", name, priority);
    } else {
        for (size_t i = 0; i < h->num_supported_types; ++i) {
            const uint32_t t = h->supported_types[i];

            // Валидация для connector-registered handlers
            if (source == HandlerSource::Connector && is_connector_blocked_type(t)) {
                LOG_WARN("register_handler '{}': type {} blocked for "
                         "connector handler — skipped", name, t);
                continue;
            }

            bus_.subscribe(t, name, make_cb, priority);
            entry.subscribed_types.push_back(t);
        }

        if (entry.subscribed_types.empty()) {
            LOG_WARN("register_handler '{}': all types blocked — handler not registered",
                     name);
            return;
        }

        LOG_INFO("Handler '{}' registered ({} types, prio={}, source={})",
                 name, entry.subscribed_types.size(), priority,
                 source == HandlerSource::Connector ? "connector" : "plugin");
    }

    std::unique_lock lock(handlers_mu_);
    handler_entries_[name] = std::move(entry);
}

// =============================================================================
// Scheme priority
// =============================================================================

void ConnectionManager::Impl::set_scheme_priority(std::vector<std::string> p) {
    LOG_TRACE("set_scheme_priority: {} schemes", p.size());
    scheme_priority_ = std::move(p);
}

} // namespace gn
