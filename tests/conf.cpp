#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "config.hpp"

namespace fs = std::filesystem;

static fs::path tmp_config(const std::string& content) {
    auto p = fs::temp_directory_path() / ("gn_cfg_test_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
    std::ofstream f(p);
    f << content;
    return p;
}

// ─── Basic get/set ─────────────────────────────────────────────────────────────

TEST(ConfigTest, DefaultsPopulatedOnConstruction) {
    Config cfg(true);  // defaults_only — skip loading config.json from CWD
    // Core defaults
    EXPECT_EQ(cfg.get_or<std::string>("core.listen_address",  "MISSING"),
              Config::Core::LISTEN_ADDRESS);
    EXPECT_EQ(cfg.get_or<int>("core.listen_port",    -1),
              (int)Config::Core::LISTEN_PORT);
    EXPECT_EQ(cfg.get_or<int>("core.io_threads",     -1),
              (int)Config::Core::IO_THREADS);
    // Logging defaults
    EXPECT_EQ(cfg.get_or<std::string>("logging.level", ""),
              Config::Logging::LEVEL);
    EXPECT_EQ(cfg.get_or<int>("logging.max_size", 0),
              Config::Logging::MAX_SIZE);
    // Security defaults
    EXPECT_EQ(cfg.get_or<int>("security.key_exchange_timeout", 0),
              Config::Security::KEY_EXCHANGE_TIMEOUT);
}

TEST(ConfigTest, SetAndGetInt) {
    Config cfg;
    cfg.set("custom.int_val", 42);
    EXPECT_EQ(cfg.get_or<int>("custom.int_val", 0), 42);
}

TEST(ConfigTest, SetAndGetBool) {
    Config cfg;
    cfg.set("custom.flag", true);
    EXPECT_EQ(cfg.get_or<bool>("custom.flag", false), true);
    cfg.set("custom.flag2", false);
    EXPECT_EQ(cfg.get_or<bool>("custom.flag2", true), false);
}

TEST(ConfigTest, SetAndGetDouble) {
    Config cfg;
    cfg.set("custom.pi", 3.14159);
    double v = cfg.get_or<double>("custom.pi", 0.0);
    EXPECT_NEAR(v, 3.14159, 1e-5);
}

TEST(ConfigTest, SetAndGetString) {
    Config cfg;
    cfg.set("custom.name", std::string("GoodNet"));
    EXPECT_EQ(cfg.get_or<std::string>("custom.name", ""), "GoodNet");
}

TEST(ConfigTest, SetAndGetPath) {
    Config cfg;
    cfg.set("custom.path", fs::path("/usr/local/share"));
    auto p = cfg.get_or<fs::path>("custom.path", {});
    EXPECT_EQ(p, fs::path("/usr/local/share"));
}

TEST(ConfigTest, GetOrReturnsDefaultForMissingKey) {
    Config cfg;
    EXPECT_EQ(cfg.get_or<int>("nonexistent.key", 99), 99);
    EXPECT_EQ(cfg.get_or<std::string>("no.key", "default"), "default");
    EXPECT_EQ(cfg.get_or<bool>("no.bool", true), true);
}

TEST(ConfigTest, OverwriteExistingKey) {
    Config cfg;
    cfg.set("x", 1);
    cfg.set("x", 2);
    EXPECT_EQ(cfg.get_or<int>("x", 0), 2);
}

TEST(ConfigTest, DottedKeyHierarchy) {
    Config cfg;
    cfg.set("a.b.c.d", 7);
    EXPECT_EQ(cfg.get_or<int>("a.b.c.d", 0), 7);
    // Different paths don't collide
    cfg.set("a.b.c.e", 8);
    EXPECT_EQ(cfg.get_or<int>("a.b.c.d", 0), 7);
    EXPECT_EQ(cfg.get_or<int>("a.b.c.e", 0), 8);
}

// ─── Type mismatch ─────────────────────────────────────────────────────────────

TEST(ConfigTest, GetOrReturnsFallbackOnTypeMismatch_IntAsString) {
    Config cfg;
    cfg.set("x", 42);
    // Asking for string where int was stored → fallback
    auto v = cfg.get_or<std::string>("x", "fallback");
    EXPECT_EQ(v, "fallback");
}

TEST(ConfigTest, GetOrReturnsFallbackOnTypeMismatch_StringAsInt) {
    Config cfg;
    cfg.set("x", std::string("hello"));
    EXPECT_EQ(cfg.get_or<int>("x", -1), -1);
}

TEST(ConfigTest, GetOrReturnsFallbackOnTypeMismatch_BoolAsDouble) {
    Config cfg;
    cfg.set("x", true);
    EXPECT_NEAR(cfg.get_or<double>("x", -1.0), -1.0, 1e-10);
}

// ─── JSON load/save ────────────────────────────────────────────────────────────

TEST(ConfigTest, LoadValidJson) {
    auto p = tmp_config(R"({"core":{"listen_port":7777},"logging":{"level":"debug"}})");
    Config cfg;
    ASSERT_TRUE(cfg.load_from_file(p));
    EXPECT_EQ(cfg.get_or<int>("core.listen_port", 0), 7777);
    EXPECT_EQ(cfg.get_or<std::string>("logging.level", ""), "debug");
    fs::remove(p);
}

TEST(ConfigTest, LoadOverridesDefaults) {
    auto p = tmp_config(R"({"core":{"listen_port":12345}})");
    Config cfg;
    cfg.load_from_file(p);
    EXPECT_EQ(cfg.get_or<int>("core.listen_port", 0), 12345);
    // Unmentioned keys still have defaults
    EXPECT_EQ(cfg.get_or<std::string>("core.listen_address", ""),
              Config::Core::LISTEN_ADDRESS);
    fs::remove(p);
}

TEST(ConfigTest, LoadInvalidJson_ReturnsFalse) {
    auto p = tmp_config("{ this is not json }}");
    Config cfg;
    bool result = cfg.load_from_file(p);
    EXPECT_FALSE(result);
    fs::remove(p);
}

TEST(ConfigTest, LoadNonExistentFile_ReturnsFalse) {
    Config cfg;
    EXPECT_FALSE(cfg.load_from_file("/nonexistent/path/config.json"));
}

TEST(ConfigTest, LoadBoolValue) {
    auto p = tmp_config(R"({"plugins":{"auto_load":false}})");
    Config cfg;
    cfg.load_from_file(p);
    EXPECT_EQ(cfg.get_or<bool>("plugins.auto_load", true), false);
    fs::remove(p);
}

TEST(ConfigTest, LoadDoubleValue) {
    auto p = tmp_config(R"({"custom":{"threshold":0.95}})");
    Config cfg;
    cfg.load_from_file(p);
    EXPECT_NEAR(cfg.get_or<double>("custom.threshold", 0.0), 0.95, 1e-5);
    fs::remove(p);
}

TEST(ConfigTest, LoadStringValue) {
    auto p = tmp_config(R"({"security":{"auth_method":"ed25519"}})");
    Config cfg;
    cfg.load_from_file(p);
    EXPECT_EQ(cfg.get_or<std::string>("security.auth_method", ""), "ed25519");
    fs::remove(p);
}

TEST(ConfigTest, LoadNestedObjectsFlattened) {
    auto p = tmp_config(R"({
        "a": { "b": { "c": 99 }, "d": "hello" },
        "e": true
    })");
    Config cfg;
    cfg.load_from_file(p);
    EXPECT_EQ(cfg.get_or<int>("a.b.c", 0), 99);
    EXPECT_EQ(cfg.get_or<std::string>("a.d", ""), "hello");
    EXPECT_EQ(cfg.get_or<bool>("e", false), true);
    fs::remove(p);
}

TEST(ConfigTest, SaveAndReload) {
    auto path = fs::temp_directory_path() / "gn_cfg_save_test.json";
    {
        Config cfg;
        cfg.set("x.y", 42);
        cfg.set("name", std::string("goodnet"));
        cfg.set("flag", true);
        cfg.save_to_file(path);
    }
    Config cfg2;
    ASSERT_TRUE(cfg2.load_from_file(path));
    EXPECT_EQ(cfg2.get_or<int>("x.y", 0), 42);
    EXPECT_EQ(cfg2.get_or<std::string>("name", ""), "goodnet");
    EXPECT_EQ(cfg2.get_or<bool>("flag", false), true);
    fs::remove(path);
}

TEST(ConfigTest, SaveToUnwritablePath_NoThrow) {
    Config cfg;
    // Attempt to save to an invalid path — should not throw
    EXPECT_NO_THROW(cfg.save_to_file("/proc/nonexistent/path/file.json"));
}

// ─── fs::path stored as string ─────────────────────────────────────────────────

TEST(ConfigTest, PathStoredInJsonReloadedAsString) {
    // Config saves fs::path as string in JSON; reload parses it back as string
    auto path = fs::temp_directory_path() / "gn_path_cfg.json";
    {
        Config cfg;
        cfg.set("plugins.base_dir", fs::path("/opt/goodnet/plugins"));
        cfg.save_to_file(path);
    }
    Config cfg2;
    cfg2.load_from_file(path);
    // After reload it comes back as string (JSON has no path type)
    auto v = cfg2.get_or<std::string>("plugins.base_dir", "");
    EXPECT_EQ(v, "/opt/goodnet/plugins");
    fs::remove(path);
}

// ─── Default constants sanity ──────────────────────────────────────────────────

TEST(ConfigDefaults, CoreValues) {
    EXPECT_FALSE(Config::Core::LISTEN_ADDRESS.empty());
    EXPECT_GT(Config::Core::LISTEN_PORT, 0u);
    EXPECT_GT(Config::Core::IO_THREADS, 0);
    EXPECT_GT(Config::Core::MAX_CONNECTIONS, 0u);
}

TEST(ConfigDefaults, LoggingValues) {
    EXPECT_FALSE(Config::Logging::LEVEL.empty());
    EXPECT_FALSE(Config::Logging::FILE.empty());
    EXPECT_GT(Config::Logging::MAX_SIZE, 0);
    EXPECT_GT(Config::Logging::MAX_FILES, 0);
}

TEST(ConfigDefaults, SecurityValues) {
    EXPECT_GT(Config::Security::KEY_EXCHANGE_TIMEOUT, 0);
    EXPECT_GT(Config::Security::MAX_AUTH_ATTEMPTS, 0);
    EXPECT_GT(Config::Security::SESSION_TIMEOUT, 0);
}

TEST(ConfigDefaults, PluginsValues) {
    EXPECT_GT(Config::Plugins::SCAN_INTERVAL, 0);
}

// ─── Edge cases ────────────────────────────────────────────────────────────────

TEST(ConfigTest, EmptyJson_DoesNotCrash) {
    auto p = tmp_config("{}");
    Config cfg;
    EXPECT_NO_THROW(cfg.load_from_file(p));
    fs::remove(p);
}

TEST(ConfigTest, NullJson_DoesNotCrash) {
    auto p = tmp_config("null");
    Config cfg;
    // null is valid JSON but not an object — load returns false
    EXPECT_NO_THROW(cfg.load_from_file(p));
    fs::remove(p);
}

TEST(ConfigTest, VeryLongKey) {
    Config cfg;
    std::string long_key(200, 'x');
    cfg.set(long_key, 1);
    EXPECT_EQ(cfg.get_or<int>(long_key, 0), 1);
}

TEST(ConfigTest, IntegerZeroStored) {
    Config cfg;
    cfg.set("zero", 0);
    EXPECT_EQ(cfg.get_or<int>("zero", -1), 0);
}

TEST(ConfigTest, EmptyStringStored) {
    Config cfg;
    cfg.set("empty", std::string(""));
    EXPECT_EQ(cfg.get_or<std::string>("empty", "default"), "");
}

TEST(ConfigTest, NegativeIntStored) {
    Config cfg;
    cfg.set("neg", -42);
    EXPECT_EQ(cfg.get_or<int>("neg", 0), -42);
}

TEST(ConfigTest, LargeIntStored) {
    Config cfg;
    cfg.set("large", 2000000000);
    EXPECT_EQ(cfg.get_or<int>("large", 0), 2000000000);
}
