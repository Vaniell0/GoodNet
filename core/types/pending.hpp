#pragma once

#include <chrono>
#include <string>
#include <vector>
#include <cstdint>

namespace gn {

// ── PendingMessage ────────────────────────────────────────────────────────────

/// Сообщение, ожидающее отправки до момента ESTABLISHED.
/// Используется для авто-доставки при send(uri, ...) на несуществующее соединение.
struct PendingMessage {
    uint32_t msg_type;
    std::vector<uint8_t> payload;
    std::chrono::steady_clock::time_point queued_at;

    PendingMessage(uint32_t type, std::vector<uint8_t> data)
        : msg_type(type)
        , payload(std::move(data))
        , queued_at(std::chrono::steady_clock::now())
    {}
};

} // namespace gn
