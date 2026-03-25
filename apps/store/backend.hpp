#pragma once
/// @file apps/store/backend.hpp
/// @brief Абстрактный backend-интерфейс для Store.
///
/// Реализуйте IStore для своего хранилища:
///   - Sqlite  (reference, в комплекте)
///   - Memory  (тесты / embedded)
///   - Dht     (Kademlia поверх GoodNet)
///   - Redis   (кластер)
///
/// Все методы вызываются из одного потока (strand handler'а).

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gn::store {

/// @brief Одна запись в хранилище.
struct Entry {
    std::string          key;
    std::vector<uint8_t> value;
    uint64_t             timestamp_us = 0;  ///< unix microseconds последнего обновления
    uint64_t             ttl_s        = 0;  ///< 0 = permanent
    uint8_t              flags        = 0;
};

/// @brief Абстрактный backend для Store.
///
/// Все методы синхронные — handler вызывает их последовательно.
/// Реализация отвечает за потокобезопасность только если
/// backend используется из нескольких потоков (не рекомендуется).
class IStore {
public:
    virtual ~IStore() = default;

    /// Записать key-value. Перезаписывает если ключ существует.
    /// @return true при успехе.
    virtual bool put(std::string_view key, std::span<const uint8_t> value,
                     uint64_t ttl_s, uint8_t flags) = 0;

    /// Получить запись по точному ключу.
    virtual std::optional<Entry> get(std::string_view key) = 0;

    /// Получить записи по префиксу ключа.
    virtual std::vector<Entry> get_prefix(std::string_view prefix,
                                          uint32_t max_results) = 0;

    /// Удалить запись.
    /// @return true если запись существовала.
    virtual bool del(std::string_view key) = 0;

    /// Записи новее since_timestamp (для sync).
    virtual std::vector<Entry> get_since(uint64_t since_timestamp,
                                         uint32_t max_results) = 0;

    /// Удалить просроченные записи.
    /// @return количество удалённых.
    virtual uint64_t cleanup_expired() = 0;
};

} // namespace gn::store
