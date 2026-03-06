#pragma once
#include "../types.h"
#include <span>
#include <vector>
#include <memory>
#include <string>
#include <cstring>
#include <stdexcept>
#include <type_traits>

namespace gn::sdk {

/// @defgroup data Message Data Types
/// @brief Typed wrappers for plugin messages.
/// @{

using RawBuffer = std::vector<uint8_t>;  ///< Owned byte buffer
using RawSpan   = std::span<const uint8_t>; ///< Non-owning byte view

/// @brief Abstract base for all transmittable messages.
class IData {
public:
    virtual ~IData() = default;

    /// @brief Serialize to a heap-allocated byte buffer.
    virtual RawBuffer serialize()                   const = 0;

    /// @brief Deserialize from a byte span. Throws on insufficient data.
    virtual void      deserialize(RawSpan buffer)         = 0;

    /// @brief Minimum valid payload size for pre-receive validation.
    virtual size_t    min_size()                    const = 0;

    /// @brief Convenience overload: deserialize from raw pointer.
    void deserialize(const void* data, size_t len) {
        deserialize(RawSpan(static_cast<const uint8_t*>(data), len));
    }
};

/// @brief Zero-copy wrapper for trivially-copyable (POD) structs.
///
/// Suitable for fixed-layout wire messages such as AUTH, HEARTBEAT,
/// and KEY_EXCHANGE payloads. Serialization is a plain memcpy.
///
/// ## Usage
/// ```cpp
/// using HeartbeatMsg = gn::sdk::PodData<HeartbeatPayload>;
/// HeartbeatMsg msg;
/// msg->seq       = ++counter;
/// msg->flags     = 0x00;  // ping
/// auto wire      = msg.serialize();
/// api_->send(ctx, uri, MSG_TYPE_HEARTBEAT, wire.data(), wire.size());
/// ```
/// @tparam T  Trivially-copyable struct, typically declared with #pragma pack(push,1)
template<typename T>
requires std::is_trivially_copyable_v<T>
class PodData final : public IData {
public:
    T value{};  ///< Payload struct; access via operator-> or operator*

    PodData() = default;
    explicit PodData(const T& v) : value(v) {}

    T*       operator->()       noexcept { return &value; }
    const T* operator->() const noexcept { return &value; }
    T&       operator* ()       noexcept { return value; }
    const T& operator* () const noexcept { return value; }

    RawBuffer serialize() const override {
        RawBuffer buf(sizeof(T));
        std::memcpy(buf.data(), &value, sizeof(T));
        return buf;
    }

    void deserialize(RawSpan buffer) override {
        if (buffer.size() < sizeof(T))
            throw std::runtime_error("PodData::deserialize: buffer too small");
        std::memcpy(&value, buffer.data(), sizeof(T));
    }

    size_t min_size() const override { return sizeof(T); }
};

/// @brief Deserialize bytes into a typed IData-derived object.
/// @tparam T  Must derive from IData
template<typename T>
requires std::derived_from<T, IData>
T from_bytes(const void* data, size_t len) {
    T obj;
    obj.deserialize(RawSpan(static_cast<const uint8_t*>(data), len));
    return obj;
}

/// @brief Serialize an IData-derived object to bytes.
/// @tparam T  Must derive from IData
template<typename T>
requires std::derived_from<T, IData>
RawBuffer to_bytes(const T& obj) { return obj.serialize(); }

/// @}  // defgroup data

} // namespace gn::sdk
