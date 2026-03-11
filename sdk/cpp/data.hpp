#pragma once
/// @file sdk/cpp/data.hpp
/// @brief Typed message wrappers for C++ plugin authors.

#include <span>
#include <vector>
#include <memory>
#include <cstring>
#include <concepts>
#include <stdexcept>
#include <type_traits>
#include "../sdk/types.h"

namespace gn::sdk {

using RawBuffer = std::vector<uint8_t>;
using RawSpan   = std::span<const uint8_t>;

// ─── IData ────────────────────────────────────────────────────────────────────

/// @brief Abstract base for all serializable message types.
class IData {
public:
    virtual ~IData() = default;
    virtual RawBuffer serialize()           const = 0;
    virtual void      deserialize(RawSpan b)      = 0;
    virtual size_t    min_size()            const = 0;

    void deserialize(const void* data, size_t len) {
        deserialize(RawSpan(static_cast<const uint8_t*>(data), len));
    }
};

// ─── PodData<T> ───────────────────────────────────────────────────────────────

/// @brief Zero-copy wrapper for trivially-copyable (POD) wire structs.
///
/// @tparam T  Trivially-copyable struct declared with #pragma pack(push,1).
///
/// ```cpp
/// using HeartbeatMsg = gn::sdk::PodData<gn::msg::HeartbeatPayload>;
/// HeartbeatMsg msg;
/// msg->seq   = ++counter_;
/// msg->flags = 0x00;  // ping
/// auto wire  = msg.serialize();
/// api_->send(ctx_, uri, MSG_TYPE_HEARTBEAT, wire.data(), wire.size());
/// ```
template<typename T>
    requires std::is_trivially_copyable_v<T>
class PodData final : public IData {
public:
    T value{};

    PodData() = default;
    explicit PodData(const T& v) : value(v) {}

    T*       operator->()       noexcept { return &value; }
    const T* operator->() const noexcept { return &value; }
    T&       operator*()        noexcept { return value; }
    const T& operator*()  const noexcept { return value; }

    RawBuffer serialize() const override {
        RawBuffer buf(sizeof(T));
        std::memcpy(buf.data(), &value, sizeof(T));
        return buf;
    }

    void deserialize(RawSpan b) override {
        if (b.size() < sizeof(T))
            throw std::runtime_error("PodData::deserialize: buffer too small");
        std::memcpy(&value, b.data(), sizeof(T));
    }

    size_t min_size() const override { return sizeof(T); }
};

// ─── Convenience free functions ───────────────────────────────────────────────

template<typename T>
    requires std::derived_from<T, IData>
T from_bytes(const void* data, size_t len) {
    T obj;
    obj.deserialize(RawSpan(static_cast<const uint8_t*>(data), len));
    return obj;
}

template<typename T>
    requires std::derived_from<T, IData>
RawBuffer to_bytes(const T& obj) { return obj.serialize(); }

} // namespace gn::sdk
