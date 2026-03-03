#include <gtest/gtest.h>
#include "config.hpp"
#include <filesystem>
#include <cstdlib>

namespace fs = std::filesystem;

// Helpers ──────────────────────────────────────────────────────────────────────

static fs::path tmp_path(const std::string& name) {
    const char* tmpdir = std::getenv("TMPDIR");
    fs::path base = tmpdir ? fs::path(tmpdir) : fs::temp_directory_path();
    return base / name;
}

// ─── Fixtures ─────────────────────────────────────────────────────────────────

class ConfigTest : public ::testing::Test {
protected:
    void SetUp() override {}
};

// ─── Basic defaults ───────────────────────────────────────────────────────────

/**
 * @brief Verify that Config(true) populates all expected default keys.
 */
TEST_F(ConfigTest, LoadDefaults) {
    Config cfg{true};
    EXPECT_EQ(cfg.get_or<int>("core.listen_port", 0), 25565);
    EXPECT_EQ(cfg.get_or<std::string>("logging.level", ""), "info");
    EXPECT_TRUE(cfg.get_or<bool>("plugins.auto_load", false));
    EXPECT_EQ(cfg.get_or<int>("core.max_connections", 0), 1000);
    EXPECT_EQ(cfg.get_or<std::string>("core.listen_address", ""), "0.0.0.0");
}

// ─── Set / get ────────────────────────────────────────────────────────────────

/**
 * @brief Round-trip set→get for every supported variant type.
 */
TEST_F(ConfigTest, SetAndGetValues) {
    Config cfg{true};

    cfg.set("test.int",    42);
    cfg.set("test.bool",   true);
    cfg.set("test.string", std::string("hello"));
    cfg.set("test.double", 3.14);
    cfg.set("test.path",   fs::path("/tmp/test"));

    EXPECT_EQ(cfg.get<int>   ("test.int")   .value(), 42);
    EXPECT_TRUE(cfg.get<bool>("test.bool")  .value());
    EXPECT_EQ(cfg.get<std::string>("test.string").value(), "hello");
    EXPECT_DOUBLE_EQ(cfg.get<double>("test.double").value(), 3.14);
    EXPECT_EQ(cfg.get<fs::path>("test.path").value(), fs::path("/tmp/test"));
}

/**
 * @brief String literal overload must store as std::string, not fs::path.
 */
TEST_F(ConfigTest, StringLiteralStoredAsString) {
    Config cfg{false};
    cfg.set("k", "value");
    EXPECT_EQ(cfg.get<std::string>("k").value(), "value");
}

// ─── JSON parsing ─────────────────────────────────────────────────────────────

/**
 * @brief Nested JSON must be reachable via dotted keys.
 */
TEST_F(ConfigTest, ParseJsonString) {
    Config cfg{true};
    const std::string json = R"({
        "core": { "listen_port": 8080, "max_connections": 500 },
        "custom": "value",
        "path_as_string": "/etc/goodnet"
    })";

    ASSERT_TRUE(cfg.load_from_string(json));

    EXPECT_EQ(cfg.get_or<int>("core.listen_port",    0),  8080);
    EXPECT_EQ(cfg.get_or<int>("core.max_connections", 0), 500);
    EXPECT_EQ(cfg.get_or<std::string>("custom", ""),      "value");

    // JSON strings must be retrievable as fs::path
    auto p = cfg.get<fs::path>("path_as_string");
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p.value(), fs::path("/etc/goodnet"));
}

/**
 * @brief Loading invalid JSON must return false without throwing.
 */
TEST_F(ConfigTest, ParseInvalidJson) {
    Config cfg{false};
    EXPECT_FALSE(cfg.load_from_string("{broken json"));
}

// ─── Missing keys ─────────────────────────────────────────────────────────────

/**
 * @brief Absent keys must return nullopt / default value gracefully.
 */
TEST_F(ConfigTest, MissingKeys) {
    Config cfg{true};
    EXPECT_FALSE(cfg.has("non_existent_key"));
    EXPECT_EQ(cfg.get<int>("non_existent_key"), std::nullopt);
    EXPECT_EQ(cfg.get_or<int>("non_existent_key", 999), 999);
}

// ─── Type mismatch ────────────────────────────────────────────────────────────

/**
 * @brief Requesting a wrong type must return nullopt (no exception).
 */
TEST_F(ConfigTest, TypeMismatch) {
    Config cfg{true};
    cfg.set("test.number", 123);

    auto val = cfg.get<std::string>("test.number");
    EXPECT_EQ(val, std::nullopt);
}

// ─── Remove ───────────────────────────────────────────────────────────────────

/**
 * @brief remove() must make the key absent.
 */
TEST_F(ConfigTest, Remove) {
    Config cfg{false};
    cfg.set("k", 1);
    ASSERT_TRUE(cfg.has("k"));
    cfg.remove("k");
    EXPECT_FALSE(cfg.has("k"));
}

// ─── File I/O ─────────────────────────────────────────────────────────────────

/**
 * @brief save_to_file → load_from_file round-trip inside $TMPDIR.
 * Uses $TMPDIR so the test works inside the Nix build sandbox.
 */
TEST_F(ConfigTest, FileOperations) {
    const fs::path test_path = tmp_path("goodnet_test_config.json");

    Config cfg{true};
    cfg.set("file.status", std::string("saved"));

    ASSERT_TRUE(cfg.save_to_file(test_path));
    ASSERT_TRUE(fs::exists(test_path));

    Config cfg2{false};
    ASSERT_TRUE(cfg2.load_from_file(test_path));
    EXPECT_EQ(cfg2.get_or<std::string>("file.status", ""), "saved");

    fs::remove(test_path);
}

/**
 * @brief Saving to a read-only path must return false gracefully.
 */
TEST_F(ConfigTest, SaveToReadOnlyFails) {
    Config cfg{false};
    cfg.set("k", 1);
    // /proc/version exists but is not writable
    EXPECT_FALSE(cfg.save_to_file("/proc/version_test_nope"));
}

// ─── JSON export ─────────────────────────────────────────────────────────────

/**
 * @brief save_to_string() must produce valid JSON containing the stored value.
 * Note: dotted keys are serialised as nested objects, so "export.key" → 
 * { "export": { "key": 100 } }. We verify the value via re-parsing.
 */
TEST_F(ConfigTest, ToJsonExport) {
    Config cfg{true};
    cfg.set("export.key", 100);

    const std::string json = cfg.save_to_string();
    ASSERT_FALSE(json.empty());

    // Re-parse and verify the value survived the round-trip
    Config cfg2{false};
    ASSERT_TRUE(cfg2.load_from_string(json));
    EXPECT_EQ(cfg2.get_or<int>("export.key", -1), 100);
}

/**
 * @brief save_to_string() output must be valid parseable JSON.
 */
TEST_F(ConfigTest, ExportIsValidJson) {
    Config cfg{true};
    cfg.set("a.b.c", 42);
    const std::string out = cfg.save_to_string();
    // load_from_string returns true only when nlohmann::json::parse succeeds
    Config tmp{false};
    EXPECT_TRUE(tmp.load_from_string(out));
}

// ─── Overwrite ────────────────────────────────────────────────────────────────

/**
 * @brief Setting the same key twice must update the value.
 */
TEST_F(ConfigTest, Overwrite) {
    Config cfg{false};
    cfg.set("k", 1);
    cfg.set("k", 2);
    EXPECT_EQ(cfg.get_or<int>("k", 0), 2);
}

// ─── all() ────────────────────────────────────────────────────────────────────

/**
 * @brief all() must expose every key that was set.
 */
TEST_F(ConfigTest, AllReturnsAllKeys) {
    Config cfg{true}; 
    size_t base_count = cfg.all().size(); // Запоминаем сколько дефолтов
    cfg.set("test_x", 1);
    cfg.set("test_y", std::string("z"));
    
    EXPECT_EQ(cfg.all().size(), base_count + 2);
    EXPECT_EQ(cfg.all().count("test_x"), 1);
    EXPECT_EQ(cfg.all().count("test_y"), 1);
}