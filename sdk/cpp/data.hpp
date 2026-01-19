#pragma once
#include "types.h"
#include <span>
#include <vector>
#include <memory>
#include <string>
#include <stdexcept>

namespace gn {

// Безопасный буфер для работы с сырыми данными
using RawBuffer = std::vector<uint8_t>;
using RawSpan = std::span<const uint8_t>;

// Интерфейс для сериализуемых объектов
class IData {
public:
    virtual ~IData() = default;
    virtual RawBuffer serialize() const = 0;
    virtual void deserialize(RawSpan buffer) = 0;
};

// Шаблонные хелперы для удобной работы
template<typename T>
requires std::derived_from<T, IData>
void unpack(T& obj, RawSpan buffer) {
    obj.deserialize(buffer);
}

template<typename T>
requires std::derived_from<T, IData>
void unpack(T& obj, const char* data, size_t len) {
    unpack(obj, RawSpan(reinterpret_cast<const uint8_t*>(data), len));
}

template<typename T>
requires std::derived_from<T, IData>
T create(const char* data, size_t len) {
    T obj;
    unpack(obj, data, len);
    return obj;
}

} // namespace gn