#pragma once
/// @file core/system_services.hpp
/// @brief Intercepts system service messages (0x0100-0x0FFF) before user handlers.
///
/// Thread safety: register_service/unregister_service are protected by
/// shared_mutex — safe to call from plugin init threads concurrently with
/// dispatch (which takes shared_lock). dispatch() is lock-free on the
/// hot path when no registration is happening.

#include <cstdint>
#include <functional>
#include <shared_mutex>
#include <span>
#include <unordered_map>

#include "../sdk/types.h"

namespace gn {

class SystemServiceDispatcher {
public:
    struct DispatchResult {
        bool intercepted;  ///< true = handled, don't pass to user handlers
    };

    using ServiceHandler = std::function<DispatchResult(
        conn_id_t id, const header_t* hdr, const endpoint_t* ep,
        std::span<const uint8_t> payload)>;

    /// Register a system service handler. Thread-safe (unique_lock).
    /// Can be called before or after Core::run_async().
    void register_service(uint16_t msg_type, ServiceHandler handler) {
        std::unique_lock lk(mu_);
        services_[msg_type] = std::move(handler);
    }

    /// Unregister a system service. Thread-safe (unique_lock).
    void unregister_service(uint16_t msg_type) {
        std::unique_lock lk(mu_);
        services_.erase(msg_type);
    }

    /// Dispatch to registered service. Thread-safe (shared_lock on hot path).
    DispatchResult dispatch(conn_id_t id, const header_t* hdr,
                           const endpoint_t* ep,
                           std::span<const uint8_t> payload) {
        std::shared_lock lk(mu_);
        auto it = services_.find(hdr->payload_type);
        if (it == services_.end())
            return {false};
        return it->second(id, hdr, ep, payload);
    }

    /// Check if a message type is in the system service range.
    static bool is_system_type(uint16_t type) noexcept {
        return type >= MSG_TYPE_SYS_BASE && type <= MSG_TYPE_SYS_MAX;
    }

private:
    mutable std::shared_mutex mu_;
    std::unordered_map<uint16_t, ServiceHandler> services_;
};

} // namespace gn
