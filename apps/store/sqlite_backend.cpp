/// @file apps/store/sqlite_backend.cpp
/// @brief Reference SQLite implementation of IStore.

#include "sqlite_backend.hpp"
#include <chrono>
#include <stdexcept>
#include <cstring>

namespace gn::store {

// ── Helpers ──────────────────────────────────────────────────────────────────

uint64_t Sqlite::now_us() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<microseconds>(system_clock::now().time_since_epoch()).count());
}

static void check_rc(int rc, sqlite3* db, const char* ctx) {
    if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW)
        throw std::runtime_error(
            std::string(ctx) + ": " + sqlite3_errmsg(db));
}

// ── Lifecycle ────────────────────────────────────────────────────────────────

Sqlite::Sqlite(const std::string& db_path) {
    int rc = sqlite3_open(db_path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        std::string err = sqlite3_errmsg(db_);
        sqlite3_close(db_);
        throw std::runtime_error("sqlite3_open: " + err);
    }

    // WAL mode для лучшей производительности
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA synchronous=NORMAL", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA foreign_keys=ON", nullptr, nullptr, nullptr);

    create_schema();
    prepare_statements();
}

Sqlite::~Sqlite() {
    finalize_statements();
    if (db_) sqlite3_close(db_);
}

void Sqlite::create_schema() {
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS store (
            key          TEXT PRIMARY KEY,
            value        BLOB NOT NULL,
            timestamp_us INTEGER NOT NULL,
            ttl_s        INTEGER NOT NULL DEFAULT 0,
            expires_at   INTEGER NOT NULL DEFAULT 0,
            flags        INTEGER NOT NULL DEFAULT 0
        );
        CREATE INDEX IF NOT EXISTS idx_store_expires
            ON store(expires_at) WHERE expires_at > 0;
        CREATE INDEX IF NOT EXISTS idx_store_timestamp
            ON store(timestamp_us);
    )";
    char* err = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string msg = err ? err : "unknown";
        sqlite3_free(err);
        throw std::runtime_error("create_schema: " + msg);
    }
}

void Sqlite::prepare_statements() {
    auto prep = [&](const char* sql, sqlite3_stmt** out) {
        check_rc(sqlite3_prepare_v2(db_, sql, -1, out, nullptr), db_, sql);
    };

    prep(R"(INSERT OR REPLACE INTO store (key, value, timestamp_us, ttl_s, expires_at, flags)
            VALUES (?1, ?2, ?3, ?4, ?5, ?6))", &stmt_put_);

    prep(R"(SELECT key, value, timestamp_us, ttl_s, flags FROM store
            WHERE key = ?1 AND (expires_at = 0 OR expires_at > ?2))", &stmt_get_);

    prep(R"(SELECT key, value, timestamp_us, ttl_s, flags FROM store
            WHERE key >= ?1 AND key < ?2
              AND (expires_at = 0 OR expires_at > ?3)
            ORDER BY key LIMIT ?4)", &stmt_prefix_);

    prep(R"(DELETE FROM store WHERE key = ?1)", &stmt_del_);

    prep(R"(SELECT key, value, timestamp_us, ttl_s, flags FROM store
            WHERE timestamp_us > ?1
              AND (expires_at = 0 OR expires_at > ?2)
            ORDER BY timestamp_us LIMIT ?3)", &stmt_since_);

    prep(R"(DELETE FROM store WHERE expires_at > 0 AND expires_at <= ?1)", &stmt_cleanup_);
}

void Sqlite::finalize_statements() {
    auto fin = [](sqlite3_stmt*& s) { if (s) { sqlite3_finalize(s); s = nullptr; } };
    fin(stmt_put_);
    fin(stmt_get_);
    fin(stmt_prefix_);
    fin(stmt_del_);
    fin(stmt_since_);
    fin(stmt_cleanup_);
}

Entry Sqlite::row_to_entry(sqlite3_stmt* stmt) const {
    Entry e;
    e.key = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    auto* blob = static_cast<const uint8_t*>(sqlite3_column_blob(stmt, 1));
    int blob_sz = sqlite3_column_bytes(stmt, 1);
    if (blob && blob_sz > 0)
        e.value.assign(blob, blob + blob_sz);
    e.timestamp_us = static_cast<uint64_t>(sqlite3_column_int64(stmt, 2));
    e.ttl_s        = static_cast<uint64_t>(sqlite3_column_int64(stmt, 3));
    e.flags        = static_cast<uint8_t>(sqlite3_column_int(stmt, 4));
    return e;
}

// ── IStore ────────────────────────────────────────────────────────────

bool Sqlite::put(std::string_view key, std::span<const uint8_t> value,
                        uint64_t ttl_s, uint8_t flags) {
    uint64_t ts = now_us();
    uint64_t expires = (ttl_s > 0) ? (ts / 1'000'000 + ttl_s) : 0;

    sqlite3_reset(stmt_put_);
    sqlite3_bind_text (stmt_put_, 1, key.data(), static_cast<int>(key.size()), SQLITE_TRANSIENT);
    sqlite3_bind_blob (stmt_put_, 2, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt_put_, 3, static_cast<int64_t>(ts));
    sqlite3_bind_int64(stmt_put_, 4, static_cast<int64_t>(ttl_s));
    sqlite3_bind_int64(stmt_put_, 5, static_cast<int64_t>(expires));
    sqlite3_bind_int  (stmt_put_, 6, flags);

    int rc = sqlite3_step(stmt_put_);
    return rc == SQLITE_DONE;
}

std::optional<Entry> Sqlite::get(std::string_view key) {
    uint64_t now_s = now_us() / 1'000'000;

    sqlite3_reset(stmt_get_);
    sqlite3_bind_text (stmt_get_, 1, key.data(), static_cast<int>(key.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt_get_, 2, static_cast<int64_t>(now_s));

    int rc = sqlite3_step(stmt_get_);
    if (rc == SQLITE_ROW)
        return row_to_entry(stmt_get_);
    return std::nullopt;
}

std::vector<Entry> Sqlite::get_prefix(std::string_view prefix,
                                              uint32_t max_results) {
    if (max_results == 0) max_results = 32;
    uint64_t now_s = now_us() / 1'000'000;

    // Prefix end: increment last byte for exclusive upper bound
    std::string end_key(prefix);
    if (!end_key.empty()) {
        end_key.back() = static_cast<char>(end_key.back() + 1);
    } else {
        end_key = "\xff"; // все ключи
    }

    sqlite3_reset(stmt_prefix_);
    sqlite3_bind_text (stmt_prefix_, 1, prefix.data(), static_cast<int>(prefix.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt_prefix_, 2, end_key.data(), static_cast<int>(end_key.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt_prefix_, 3, static_cast<int64_t>(now_s));
    sqlite3_bind_int  (stmt_prefix_, 4, static_cast<int>(max_results));

    std::vector<Entry> result;
    while (sqlite3_step(stmt_prefix_) == SQLITE_ROW)
        result.push_back(row_to_entry(stmt_prefix_));
    return result;
}

bool Sqlite::del(std::string_view key) {
    sqlite3_reset(stmt_del_);
    sqlite3_bind_text(stmt_del_, 1, key.data(), static_cast<int>(key.size()), SQLITE_TRANSIENT);
    sqlite3_step(stmt_del_);
    return sqlite3_changes(db_) > 0;
}

std::vector<Entry> Sqlite::get_since(uint64_t since_timestamp,
                                             uint32_t max_results) {
    if (max_results == 0) max_results = 256;
    uint64_t now_s = now_us() / 1'000'000;

    sqlite3_reset(stmt_since_);
    sqlite3_bind_int64(stmt_since_, 1, static_cast<int64_t>(since_timestamp));
    sqlite3_bind_int64(stmt_since_, 2, static_cast<int64_t>(now_s));
    sqlite3_bind_int  (stmt_since_, 3, static_cast<int>(max_results));

    std::vector<Entry> result;
    while (sqlite3_step(stmt_since_) == SQLITE_ROW)
        result.push_back(row_to_entry(stmt_since_));
    return result;
}

uint64_t Sqlite::cleanup_expired() {
    uint64_t now_s = now_us() / 1'000'000;

    sqlite3_reset(stmt_cleanup_);
    sqlite3_bind_int64(stmt_cleanup_, 1, static_cast<int64_t>(now_s));
    sqlite3_step(stmt_cleanup_);
    return static_cast<uint64_t>(sqlite3_changes(db_));
}

} // namespace gn::store
