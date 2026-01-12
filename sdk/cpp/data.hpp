#pragma once
#include "types.h"
#include <nlohmann/json.hpp>
#include <span>
#include <vector>
#include <memory>
#include <string>
#include <stdexcept>

namespace gn::sdk {

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

// JSON-реализация для удобной работы с данными
class JsonData : public IData {
public:
    nlohmann::json data = nlohmann::json::object();

    RawBuffer serialize() const override {
        std::string json_str = data.dump();
        return RawBuffer(json_str.begin(), json_str.end());
    }

    void deserialize(RawSpan buffer) override {
        std::string json_str(reinterpret_cast<const char*>(buffer.data()), buffer.size());
        data = nlohmann::json::parse(json_str);
    }

    template<typename T>
    T get(const std::string& key, const T& default_value = T()) const {
        if (data.contains(key)) {
            try {
                return data[key].get<T>();
            } catch (...) {
                return default_value;
            }
        }
        return default_value;
    }

    template<typename T>
    void set(const std::string& key, const T& value) {
        data[key] = value;
    }
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

} // namespace gn::sdk