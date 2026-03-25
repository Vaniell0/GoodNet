#pragma once
/// @file apps/store/handler.hpp
/// @brief Wire protocol handler для Store.
///
/// Подписывается на MSG_TYPE_SYS_STORE_* через core.subscribe(),
/// транслирует wire-запросы в вызовы IStore.

#include "backend.hpp"

#include "core.hpp"
#include "signals.hpp"

#include <mutex>
#include <vector>

namespace gn::store {

/// @brief Wire protocol handler: maps STORE_* messages to IStore.
class StoreHandler {
public:
    StoreHandler(gn::Core& core, IStore& backend);
    ~StoreHandler();

    /// Подписаться на все STORE_* типы сообщений.
    void start();

    /// Отписаться.
    void stop();

    /// Удалить подписки для отключённого пира.
    void on_disconnect(conn_id_t id);

private:
    gn::Core&       core_;
    IStore&  backend_;
    std::vector<uint64_t> sub_ids_;

    // ── Wire handlers ────────────────────────────────────────────────────────

    propagation_t on_put(std::string_view name, std::shared_ptr<header_t> hdr,
                         const endpoint_t* ep, PacketData data);

    propagation_t on_get(std::string_view name, std::shared_ptr<header_t> hdr,
                         const endpoint_t* ep, PacketData data);

    propagation_t on_delete(std::string_view name, std::shared_ptr<header_t> hdr,
                            const endpoint_t* ep, PacketData data);

    propagation_t on_subscribe(std::string_view name, std::shared_ptr<header_t> hdr,
                               const endpoint_t* ep, PacketData data);

    propagation_t on_sync(std::string_view name, std::shared_ptr<header_t> hdr,
                          const endpoint_t* ep, PacketData data);

    // ── Subscriptions ────────────────────────────────────────────────────────

    struct Sub {
        conn_id_t   conn_id;
        std::string key;
        uint8_t     query_type;  // 0 = exact, 1 = prefix
    };

    std::mutex       sub_mu_;
    std::vector<Sub> subscriptions_;

    void notify_subscribers(const Entry& entry, uint8_t event);

    // ── Serialization helpers ────────────────────────────────────────────────

    std::vector<uint8_t> serialize_result(uint64_t request_id,
                                          const std::vector<Entry>& entries,
                                          uint8_t status);

    std::vector<uint8_t> serialize_entry(const Entry& e);

    std::vector<uint8_t> serialize_notify(const Entry& entry, uint8_t event);
};

} // namespace gn::store
