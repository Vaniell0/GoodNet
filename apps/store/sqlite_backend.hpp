#pragma once
/// @file apps/store/sqlite_backend.hpp
/// @brief Reference SQLite implementation of IStore.

#include "backend.hpp"
#include <sqlite3.h>

namespace gn::store {

/// @brief SQLite-based key-value backend (reference implementation).
///
/// Одна таблица `store` с generic key-value записями.
/// Namespace — часть ключа: "peer/abc123", "service/chat".
class Sqlite final : public IStore {
public:
    /// @param db_path Путь к файлу SQLite (создаётся если не существует).
    explicit Sqlite(const std::string& db_path);
    ~Sqlite() override;

    Sqlite(const Sqlite&) = delete;
    Sqlite& operator=(const Sqlite&) = delete;

    bool                 put(std::string_view key, std::span<const uint8_t> value,
                             uint64_t ttl_s, uint8_t flags) override;
    std::optional<Entry> get(std::string_view key) override;
    std::vector<Entry>   get_prefix(std::string_view prefix,
                                    uint32_t max_results) override;
    bool                 del(std::string_view key) override;
    std::vector<Entry>   get_since(uint64_t since_timestamp,
                                   uint32_t max_results) override;
    uint64_t             cleanup_expired() override;

private:
    sqlite3* db_ = nullptr;

    // Prepared statements (кешируемые для производительности)
    sqlite3_stmt* stmt_put_       = nullptr;
    sqlite3_stmt* stmt_get_       = nullptr;
    sqlite3_stmt* stmt_prefix_    = nullptr;
    sqlite3_stmt* stmt_del_       = nullptr;
    sqlite3_stmt* stmt_since_     = nullptr;
    sqlite3_stmt* stmt_cleanup_   = nullptr;

    void create_schema();
    void prepare_statements();
    void finalize_statements();
    Entry row_to_entry(sqlite3_stmt* stmt) const;

    static uint64_t now_us();
};

} // namespace gn::store