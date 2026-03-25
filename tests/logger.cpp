/// @file tests/logger.cpp
/// @brief Unit tests for Logger static setters/getters.

#include <gtest/gtest.h>
#include "logger.hpp"

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 1: Log level
// ═══════════════════════════════════════════════════════════════════════════════

TEST(LoggerTest, SetLogLevel_InfoRoundtrip) {
    // Setter/getter roundtrip (can't test default — static state shared across tests)
    auto prev = Logger::get_log_level();
    Logger::set_log_level("info");
    EXPECT_EQ(Logger::get_log_level(), "info");
    Logger::set_log_level(prev);
}

TEST(LoggerTest, SetLogLevel_Valid) {
    Logger::set_log_level("debug");
    EXPECT_EQ(Logger::get_log_level(), "debug");
    // Restore
    Logger::set_log_level("info");
}

TEST(LoggerTest, SetLogLevel_ArbitraryString) {
    // Setters don't validate — they store as-is
    auto prev = Logger::get_log_level();
    Logger::set_log_level("garbage");
    EXPECT_EQ(Logger::get_log_level(), "garbage");
    // Restore
    Logger::set_log_level(prev);
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 2: Setter/Getter roundtrips
// ═══════════════════════════════════════════════════════════════════════════════

TEST(LoggerTest, SetLogFile_GetLogFile) {
    auto prev = Logger::get_log_file();
    Logger::set_log_file("/tmp/gn_test.log");
    EXPECT_EQ(Logger::get_log_file(), "/tmp/gn_test.log");
    Logger::set_log_file(prev);
}

TEST(LoggerTest, SetMaxSize_GetMaxSize) {
    auto prev = Logger::get_max_size();
    Logger::set_max_size(1024 * 1024);
    EXPECT_EQ(Logger::get_max_size(), 1024u * 1024u);
    Logger::set_max_size(prev);
}

TEST(LoggerTest, SetMaxFiles_GetMaxFiles) {
    auto prev = Logger::get_max_files();
    Logger::set_max_files(10);
    EXPECT_EQ(Logger::get_max_files(), 10);
    Logger::set_max_files(prev);
}

TEST(LoggerTest, SetProjectRoot_GetProjectRoot) {
    auto prev = Logger::get_project_root();
    Logger::set_project_root("/opt/goodnet");
    EXPECT_EQ(Logger::get_project_root(), "/opt/goodnet");
    Logger::set_project_root(prev);
}

TEST(LoggerTest, SetStripExtension_GetStripExtension) {
    auto prev = Logger::get_strip_extension();
    Logger::set_strip_extension(!prev);
    EXPECT_EQ(Logger::get_strip_extension(), !prev);
    Logger::set_strip_extension(prev);
}

TEST(LoggerTest, SetSourceDetailMode_GetSourceDetailMode) {
    auto prev = Logger::get_source_detail_mode();
    Logger::set_source_detail_mode(2);
    EXPECT_EQ(Logger::get_source_detail_mode(), 2);
    Logger::set_source_detail_mode(prev);
}

TEST(LoggerTest, SetConsolePattern_GetConsolePattern) {
    auto prev = Logger::get_console_pattern();
    Logger::set_console_pattern("[%Y-%m-%d] %v");
    EXPECT_EQ(Logger::get_console_pattern(), "[%Y-%m-%d] %v");
    Logger::set_console_pattern(prev);
}

TEST(LoggerTest, SetFilePattern_GetFilePattern) {
    auto prev = Logger::get_file_pattern();
    Logger::set_file_pattern("[%H:%M] %v");
    EXPECT_EQ(Logger::get_file_pattern(), "[%H:%M] %v");
    Logger::set_file_pattern(prev);
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 3: Logger instance
// ═══════════════════════════════════════════════════════════════════════════════

TEST(LoggerTest, LoggerGet_ReturnsNonNull) {
    auto logger = Logger::get();
    EXPECT_NE(logger, nullptr);
}

TEST(LoggerTest, LoggerGet_ReturnsSameInstance) {
    auto a = Logger::get();
    auto b = Logger::get();
    EXPECT_EQ(a.get(), b.get());
}

TEST(LoggerTest, ShutdownDoesNotCrash) {
    // Shutdown is safe to call; once_flag prevents re-init so we only verify no throw
    EXPECT_NO_THROW(Logger::shutdown());
}
