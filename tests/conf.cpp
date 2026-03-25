#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>

#include "config.hpp"

namespace fs = std::filesystem;

static fs::path tmp_config(const std::string& content) {
    auto p = fs::temp_directory_path() / ("gn_cfg_test_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
    std::ofstream f(p);
    f << content;
    return p;
}

// ─── Default values ─────────────────────────────────────────────────────────

TEST(ConfigTest, DefaultsPopulatedOnConstruction) {
    Config cfg(true);
    // Core
    EXPECT_EQ(cfg.core.listen_address, "0.0.0.0");
    EXPECT_EQ(cfg.core.listen_port, 25565);
    EXPECT_EQ(cfg.core.io_threads, 0);
    EXPECT_EQ(cfg.core.max_connections, 1000);
    // Logging
    EXPECT_EQ(cfg.logging.level, "info");
    EXPECT_TRUE(cfg.logging.file.empty());
    EXPECT_EQ(cfg.logging.max_size, 10 * 1024 * 1024);
    EXPECT_EQ(cfg.logging.max_files, 5);
    // Security
    EXPECT_EQ(cfg.security.key_exchange_timeout, 30);
    EXPECT_EQ(cfg.security.max_auth_attempts, 3);
    EXPECT_EQ(cfg.security.session_timeout, 3600);
    // Compression
    EXPECT_TRUE(cfg.compression.enabled);
    EXPECT_EQ(cfg.compression.threshold, 512);
    EXPECT_EQ(cfg.compression.level, 1);
    // Plugins
    EXPECT_TRUE(cfg.plugins.base_dir.empty());
    EXPECT_TRUE(cfg.plugins.auto_load);
    EXPECT_EQ(cfg.plugins.scan_interval, 300);
    // Identity
    EXPECT_EQ(cfg.identity.dir, "~/.goodnet");
    EXPECT_TRUE(cfg.identity.ssh_key_path.empty());
    EXPECT_TRUE(cfg.identity.use_machine_id);
}

TEST(ConfigDefaults, CoreValues) {
    Config cfg(true);
    EXPECT_EQ(cfg.core.listen_address, "0.0.0.0");
    EXPECT_EQ(cfg.core.listen_port, 25565);
    EXPECT_EQ(cfg.core.max_connections, 1000);
}

TEST(ConfigDefaults, LoggingValues) {
    Config cfg(true);
    EXPECT_EQ(cfg.logging.level, "info");
    EXPECT_EQ(cfg.logging.max_size, 10 * 1024 * 1024);
    EXPECT_EQ(cfg.logging.max_files, 5);
}

TEST(ConfigDefaults, SecurityValues) {
    Config cfg(true);
    EXPECT_EQ(cfg.security.key_exchange_timeout, 30);
    EXPECT_EQ(cfg.security.max_auth_attempts, 3);
    EXPECT_EQ(cfg.security.session_timeout, 3600);
}

TEST(ConfigDefaults, PluginsValues) {
    Config cfg(true);
    EXPECT_EQ(cfg.plugins.scan_interval, 300);
    EXPECT_TRUE(cfg.plugins.auto_load);
}

// ─── Direct field access ────────────────────────────────────────────────────

TEST(ConfigTest, SetAndReadFields) {
    Config cfg(true);
    cfg.core.listen_port = 7777;
    EXPECT_EQ(cfg.core.listen_port, 7777);

    cfg.logging.level = "debug";
    EXPECT_EQ(cfg.logging.level, "debug");

    cfg.compression.enabled = false;
    EXPECT_FALSE(cfg.compression.enabled);

    cfg.identity.dir = "/custom/path";
    EXPECT_EQ(cfg.identity.dir, "/custom/path");
}

// ─── JSON load ──────────────────────────────────────────────────────────────

TEST(ConfigTest, LoadValidJson) {
    auto p = tmp_config(R"({"core":{"listen_port":7777},"logging":{"level":"debug"}})");
    Config cfg(true);
    ASSERT_TRUE(cfg.load_from_file(p));
    EXPECT_EQ(cfg.core.listen_port, 7777);
    EXPECT_EQ(cfg.logging.level, "debug");
    fs::remove(p);
}

TEST(ConfigTest, LoadOverridesDefaults) {
    auto p = tmp_config(R"({"core":{"listen_port":12345}})");
    Config cfg(true);
    cfg.load_from_file(p);
    EXPECT_EQ(cfg.core.listen_port, 12345);
    // Unmentioned keys still have defaults
    EXPECT_EQ(cfg.core.listen_address, "0.0.0.0");
    EXPECT_EQ(cfg.logging.level, "info");
    fs::remove(p);
}

TEST(ConfigTest, LoadInvalidJson_ReturnsFalse) {
    auto p = tmp_config("{ this is not json }}");
    Config cfg(true);
    bool result = cfg.load_from_file(p);
    EXPECT_FALSE(result);
    fs::remove(p);
}

TEST(ConfigTest, LoadNonExistentFile_ReturnsFalse) {
    Config cfg(true);
    EXPECT_FALSE(cfg.load_from_file("/nonexistent/path/config.json"));
}

TEST(ConfigTest, LoadBoolValue) {
    auto p = tmp_config(R"({"plugins":{"auto_load":false}})");
    Config cfg(true);
    cfg.load_from_file(p);
    EXPECT_FALSE(cfg.plugins.auto_load);
    fs::remove(p);
}

TEST(ConfigTest, LoadAllSections) {
    auto p = tmp_config(R"({
        "core": {"listen_address":"10.0.0.1","listen_port":8080,"io_threads":4,"max_connections":500},
        "logging": {"level":"warn","file":"/var/log/gn.log","max_size":5242880,"max_files":3},
        "security": {"key_exchange_timeout":60,"max_auth_attempts":5,"session_timeout":7200},
        "compression": {"enabled":false,"threshold":1024,"level":3},
        "plugins": {"base_dir":"/opt/plugins","auto_load":false,"scan_interval":600,"extra_dirs":"/a;/b"},
        "identity": {"dir":"/home/test/.gn","ssh_key_path":"/home/test/.ssh/id","use_machine_id":false}
    })");
    Config cfg(true);
    ASSERT_TRUE(cfg.load_from_file(p));

    EXPECT_EQ(cfg.core.listen_address, "10.0.0.1");
    EXPECT_EQ(cfg.core.listen_port, 8080);
    EXPECT_EQ(cfg.core.io_threads, 4);
    EXPECT_EQ(cfg.core.max_connections, 500);

    EXPECT_EQ(cfg.logging.level, "warn");
    EXPECT_EQ(cfg.logging.file, "/var/log/gn.log");
    EXPECT_EQ(cfg.logging.max_size, 5242880);
    EXPECT_EQ(cfg.logging.max_files, 3);

    EXPECT_EQ(cfg.security.key_exchange_timeout, 60);
    EXPECT_EQ(cfg.security.max_auth_attempts, 5);
    EXPECT_EQ(cfg.security.session_timeout, 7200);

    EXPECT_FALSE(cfg.compression.enabled);
    EXPECT_EQ(cfg.compression.threshold, 1024);
    EXPECT_EQ(cfg.compression.level, 3);

    EXPECT_EQ(cfg.plugins.base_dir, "/opt/plugins");
    EXPECT_FALSE(cfg.plugins.auto_load);
    EXPECT_EQ(cfg.plugins.scan_interval, 600);
    EXPECT_EQ(cfg.plugins.extra_dirs, "/a;/b");

    EXPECT_EQ(cfg.identity.dir, "/home/test/.gn");
    EXPECT_EQ(cfg.identity.ssh_key_path, "/home/test/.ssh/id");
    EXPECT_FALSE(cfg.identity.use_machine_id);

    fs::remove(p);
}

TEST(ConfigTest, LoadFromString) {
    Config cfg(true);
    EXPECT_TRUE(cfg.load_from_string(R"({"core":{"listen_port":9999}})"));
    EXPECT_EQ(cfg.core.listen_port, 9999);
}

// ─── JSON save & round-trip ─────────────────────────────────────────────────

TEST(ConfigTest, SaveAndReload) {
    auto path = fs::temp_directory_path() / "gn_cfg_save_test.json";
    {
        Config cfg(true);
        cfg.core.listen_port = 42;
        cfg.logging.level    = "debug";
        cfg.compression.enabled = false;
        cfg.save_to_file(path);
    }
    Config cfg2(true);
    ASSERT_TRUE(cfg2.load_from_file(path));
    EXPECT_EQ(cfg2.core.listen_port, 42);
    EXPECT_EQ(cfg2.logging.level, "debug");
    EXPECT_FALSE(cfg2.compression.enabled);
    fs::remove(path);
}

TEST(ConfigTest, SaveToString_LoadFromString) {
    Config cfg(true);
    cfg.core.listen_port    = 1234;
    cfg.identity.dir        = "/test";
    cfg.plugins.auto_load   = false;
    auto json = cfg.save_to_string();
    EXPECT_FALSE(json.empty());

    Config cfg2(true);
    EXPECT_TRUE(cfg2.load_from_string(json));
    EXPECT_EQ(cfg2.core.listen_port, 1234);
    EXPECT_EQ(cfg2.identity.dir, "/test");
    EXPECT_FALSE(cfg2.plugins.auto_load);
}

TEST(ConfigTest, SaveToUnwritablePath_NoThrow) {
    Config cfg(true);
    EXPECT_NO_THROW(cfg.save_to_file("/proc/nonexistent/path/file.json"));
}

// ─── Reload ─────────────────────────────────────────────────────────────────

TEST(ConfigTest, Reload_ReloadsFromFile) {
    auto path = fs::temp_directory_path() / "gn_reload_test.json";
    {
        Config cfg(true);
        cfg.core.listen_port = 10;
        cfg.save_to_file(path);
        cfg.load_from_file(path);
    }
    {
        std::ofstream f(path);
        f << R"({"core":{"listen_port":99}})";
    }
    Config cfg2(true);
    cfg2.load_from_file(path);
    EXPECT_EQ(cfg2.core.listen_port, 99);

    {
        std::ofstream f(path);
        f << R"({"core":{"listen_port":77}})";
    }
    cfg2.reload();
    EXPECT_EQ(cfg2.core.listen_port, 77);
    fs::remove(path);
}

// ─── Edge cases ─────────────────────────────────────────────────────────────

TEST(ConfigTest, EmptyJson_DoesNotCrash) {
    auto p = tmp_config("{}");
    Config cfg(true);
    EXPECT_NO_THROW(cfg.load_from_file(p));
    // Defaults preserved
    EXPECT_EQ(cfg.core.listen_port, 25565);
    fs::remove(p);
}

TEST(ConfigTest, NullJson_DoesNotCrash) {
    auto p = tmp_config("null");
    Config cfg(true);
    EXPECT_NO_THROW(cfg.load_from_file(p));
    fs::remove(p);
}

TEST(ConfigTest, UnknownKeys_Ignored) {
    auto p = tmp_config(R"({"unknown_section":{"foo":"bar"},"core":{"listen_port":5555}})");
    Config cfg(true);
    EXPECT_TRUE(cfg.load_from_file(p));
    EXPECT_EQ(cfg.core.listen_port, 5555);
    fs::remove(p);
}

TEST(ConfigTest, PartialSection_OnlyOverridesPresent) {
    auto p = tmp_config(R"({"core":{"listen_port":3000}})");
    Config cfg(true);
    cfg.load_from_file(p);
    EXPECT_EQ(cfg.core.listen_port, 3000);
    EXPECT_EQ(cfg.core.listen_address, "0.0.0.0");  // untouched default
    EXPECT_EQ(cfg.core.io_threads, 0);               // untouched default
    fs::remove(p);
}

// ─── get_raw (CAPI compat) ─────────────────────────────────────────────────

TEST(ConfigTest, GetRaw_KnownKeys) {
    Config cfg(true);
    cfg.core.listen_port      = 9000;
    cfg.logging.level         = "trace";
    cfg.compression.enabled   = false;
    cfg.identity.dir          = "/opt/gn";

    EXPECT_EQ(cfg.get_raw("core.listen_port"),      "9000");
    EXPECT_EQ(cfg.get_raw("core.listen_address"),    "0.0.0.0");
    EXPECT_EQ(cfg.get_raw("logging.level"),          "trace");
    EXPECT_EQ(cfg.get_raw("compression.enabled"),    "false");
    EXPECT_EQ(cfg.get_raw("compression.threshold"),  "512");
    EXPECT_EQ(cfg.get_raw("identity.dir"),           "/opt/gn");
    EXPECT_EQ(cfg.get_raw("plugins.auto_load"),      "true");
    EXPECT_EQ(cfg.get_raw("security.session_timeout"), "3600");
}

TEST(ConfigTest, GetRaw_UnknownKey_ReturnsNullopt) {
    Config cfg(true);
    EXPECT_FALSE(cfg.get_raw("nonexistent.key").has_value());
    EXPECT_FALSE(cfg.get_raw("").has_value());
    EXPECT_FALSE(cfg.get_raw("core").has_value());
}

TEST(ConfigTest, GetRaw_BoolValues) {
    Config cfg(true);
    cfg.compression.enabled = true;
    EXPECT_EQ(cfg.get_raw("compression.enabled"), "true");
    cfg.compression.enabled = false;
    EXPECT_EQ(cfg.get_raw("compression.enabled"), "false");

    cfg.identity.use_machine_id = false;
    EXPECT_EQ(cfg.get_raw("identity.use_machine_id"), "false");
}
