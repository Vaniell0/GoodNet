#pragma once
/// @file sdk/cpp/data.hpp
/// @brief Typed message wrappers for C++ plugin and core use.
///
/// Provides serialization abstractions for wire messages:
///   - `IData`       — abstract base for all serializable message types
///   - `PodData<T>`  — zero-copy wrapper for trivially-copyable (packed) structs
///   - `VarData`     — variable-length byte buffer for schema-less payloads
///
/// ## Usage
/// @code
/// // Fixed-layout message:
/// using Heartbeat = gn::sdk::PodData<gn::msg::HeartbeatPayload>;
/// Heartbeat msg;
/// msg->seq = ++seq_;
/// auto bytes = msg.serialize();
///
/// // Variable-length message:
/// gn::sdk::VarData chat(text.data(), text.size());
/// api->send(ctx, uri, MSG_TYPE_CHAT, chat.bytes.data(), chat.bytes.size());
///
/// // Deserialize from wire:
/// auto hb = gn::sdk::from_bytes<Heartbeat>(payload, len);
/// @endcode

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
    /// @brief Owned byte buffer for serialized messages.
    using RawBuffer = std::vector<uint8_t>;

    /// @brief Non-owning view of raw bytes.
    using RawSpan   = std::span<const uint8_t>;
}

/// @brief Shared packet data buffer for zero-copy dispatch through the handler chain.
using PacketData = std::shared_ptr<sdk::RawBuffer>;

namespace sdk {

// ── IData ─────────────────────────────────────────────────────────────────────

/// @brief Abstract base for all serializable message types.
///
/// Subclasses must implement serialize(), deserialize(), and min_size().
/// The core uses these for type-safe packet encoding/decoding.
class IData {
public:
    virtual ~IData()                              = default;

    /// @brief Serialize to a byte buffer for transmission.
    virtual RawBuffer serialize()           const = 0;

    /// @brief Deserialize from a byte span.
    /// @throws std::runtime_error if buffer is too small.
    virtual void      deserialize(RawSpan b)      = 0;

    /// @brief Minimum valid buffer size for deserialization.
    virtual size_t    min_size()            const = 0;

    /// @brief Convenience overload for raw pointer + length.
    void deserialize(const void* data, size_t len) {
        deserialize(RawSpan(static_cast<const uint8_t*>(data), len));
    }
};

// ── PodData<T> ────────────────────────────────────────────────────────────────

/// @brief Zero-copy wrapper for trivially-copyable (packed) wire structs.
///
/// Provides direct member access via operator-> and serialization via memcpy.
/// The template parameter T must be trivially copyable (typically a `#pragma pack(push,1)` struct).
///
/// @tparam T  Trivially-copyable payload struct (e.g. HeartbeatPayload).
template<typename T>
    requires std::is_trivially_copyable_v<T>
class PodData final : public IData {
public:
    T value{};  ///< The payload struct — direct access for reading/writing fields.

    PodData() = default;
    explicit PodData(const T& v) : value(v) {}

    /// @brief Direct member access (e.g. `msg->seq = 42`).
    T*       operator->()       noexcept { return &value; }
    const T* operator->() const noexcept { return &value; }
    T&       operator*()        noexcept { return value; }
    const T& operator*()  const noexcept { return value; }

    /// @brief Raw byte pointer for C API interop.
    const void* bytes() const noexcept { return &value; }

    /// @brief Wire size in bytes.
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

/// @brief Variable-length byte buffer message.
///
/// For payloads without a fixed schema (e.g. chat text, file chunks).
class VarData final : public IData {
public:
    RawBuffer bytes;  ///< Payload bytes.

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

/// @brief Deserialize an IData subclass from raw bytes.
/// @tparam T  IData subclass (e.g. PodData<HeartbeatPayload>).
/// @throws std::runtime_error if buffer is too small.
template<typename T>
    requires std::derived_from<T, IData>
T from_bytes(const void* data, size_t len) {
    T obj;
    obj.deserialize(RawSpan(static_cast<const uint8_t*>(data), len));
    return obj;
}

/// @brief Serialize an IData subclass to a byte buffer.
template<typename T>
    requires std::derived_from<T, IData>
RawBuffer to_bytes(const T& obj) { return obj.serialize(); }

/// @brief Wrap raw network bytes in a shared buffer for zero-copy dispatch.
///
/// Used internally by the core to share packet data across the handler chain
/// without copying per handler.
inline PacketData make_packet(const void* data, size_t len) {
    auto buf = std::make_shared<RawBuffer>(
        static_cast<const uint8_t*>(data),
        static_cast<const uint8_t*>(data) + len);
    return buf;
}

} // namespace sdk
} // namespace gn
