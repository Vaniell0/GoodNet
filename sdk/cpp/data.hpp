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

// ─── Базовые типы буферов ─────────────────────────────────────────────────────

using RawBuffer = std::vector<uint8_t>;
using RawSpan   = std::span<const uint8_t>;

// ─── IData ────────────────────────────────────────────────────────────────────
//
// Интерфейс всех передаваемых сообщений.
// Две специализации:
//   PodData<T>  — фиксированный POD-layout (AUTH, HEARTBEAT, ключи и т.д.)

class IData {
public:
    virtual ~IData() = default;

    virtual RawBuffer serialize()                 const = 0;
    virtual void      deserialize(RawSpan buffer)       = 0;

    // Хелпер: deserialize из сырого указателя
    void deserialize(const void* data, size_t len) {
        deserialize(RawSpan(static_cast<const uint8_t*>(data), len));
    }

    // Минимальный размер валидного payload (для проверки на приёмнике)
    virtual size_t min_size() const = 0;
};

// ─── PodData<T> ───────────────────────────────────────────────────────────────
//
// Обёртка над #pragma pack(push,1) структурами из sdk/types.h
// Никакого копирования не делает при сериализации — просто memcpy.
//
// Использование:
//   using AuthMsg = PodData<auth_payload_t>;
//   AuthMsg msg;
//   msg->user_pubkey = ...;
//   auto buf = msg.serialize();

template<typename T>
requires std::is_trivially_copyable_v<T>
class PodData final : public IData {
public:
    T value{};

    PodData() = default;
    explicit PodData(const T& v) : value(v) {}

    // Arrow / dereference для удобного доступа к полям
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

// ─── Хелперы ─────────────────────────────────────────────────────────────────

// Десериализовать из raw buffer
template<typename T>
requires std::derived_from<T, IData>
T from_bytes(const void* data, size_t len) {
    T obj;
    obj.deserialize(RawSpan(static_cast<const uint8_t*>(data), len));
    return obj;
}

// Сериализовать в raw buffer (move-friendly)
template<typename T>
requires std::derived_from<T, IData>
RawBuffer to_bytes(const T& obj) { return obj.serialize(); }

} // namespace gn::sdk
