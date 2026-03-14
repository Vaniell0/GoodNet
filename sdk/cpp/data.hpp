#pragma once
/// @file sdk/cpp/data.hpp
/// @brief Typed message wrappers for C++ plugin and core use.

#include <concepts>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <vector>

#include "../sdk/types.h"

namespace gn {

namespace sdk {
    using RawBuffer = std::vector<uint8_t>;
    using RawSpan   = std::span<const uint8_t>;
}

using PacketData = std::shared_ptr<sdk::RawBuffer>;

namespace sdk {

// ── IData ─────────────────────────────────────────────────────────────────────

/// Abstract base for all serializable message types.
class IData {
public:
    virtual ~IData()                              = default;
    virtual RawBuffer serialize()           const = 0;
    virtual void      deserialize(RawSpan b)      = 0;
    virtual size_t    min_size()            const = 0;

    void deserialize(const void* data, size_t len) {
        deserialize(RawSpan(static_cast<const uint8_t*>(data), len));
    }
};

// ── PodData<T> ────────────────────────────────────────────────────────────────

/// Zero-copy wrapper for trivially-copyable (packed) wire structs.
///
/// ```cpp
/// using Heartbeat = gn::sdk::PodData<gn::msg::HeartbeatPayload>;
/// Heartbeat msg;
/// msg->seq = ++seq_;
/// api_->send(ctx_, uri, MSG_TYPE_HEARTBEAT, msg.bytes(), msg.size());
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

    const void* bytes() const noexcept { return &value; }
    size_t      size()  const noexcept { return sizeof(T); }

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

// ── VarData ───────────────────────────────────────────────────────────────────

/// Variable-length byte buffer message — for payloads without a fixed schema.
class VarData final : public IData {
public:
    RawBuffer bytes;

    VarData() = default;
    explicit VarData(RawBuffer b) : bytes(std::move(b)) {}
    explicit VarData(const void* data, size_t len)
        : bytes(static_cast<const uint8_t*>(data),
                static_cast<const uint8_t*>(data) + len) {}

    RawBuffer serialize() const override { return bytes; }

    void deserialize(RawSpan b) override {
        bytes.assign(b.begin(), b.end());
    }

    size_t min_size() const override { return 0; }
};

// ── Convenience free functions ────────────────────────────────────────────────

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

/// Wrap raw network bytes in a PacketData shared buffer (zero-copy for dispatch).
inline PacketData make_packet(const void* data, size_t len) {
    auto buf = std::make_shared<RawBuffer>(
        static_cast<const uint8_t*>(data),
        static_cast<const uint8_t*>(data) + len);
    return buf;
}

} // namespace sdk
} // namespace gn