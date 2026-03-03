#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <sodium.h>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <nlohmann/json.hpp>
#include "core/pluginManager.hpp"

namespace fs = std::filesystem;

// ─── Fixture ──────────────────────────────────────────────────────────────────

class PluginManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_root = fs::temp_directory_path() / "goodnet_test_run";
        fs::create_directories(test_root / "handlers");
        fs::create_directories(test_root / "connectors");

        static host_api_t api{};
        api.internal_logger = nullptr;
        pm = std::make_unique<gn::PluginManager>(&api, test_root);
    }

    void TearDown() override {
        pm.reset();  // unload_all() before removing files
        fs::remove_all(test_root);
    }

    /**
     * @brief Copy a plugin .so into the correct subdirectory and write its
     * .so.json integrity manifest.
     *
     * @param src_path  CMake-injected path to the compiled mock .so
     * @param subdir    "handlers" or "connectors"
     * @param name      Value stored in meta.name inside the manifest
     * @return          Path to the installed .so
     */
    fs::path install_mock(const char* src_path,
                          const std::string& subdir,
                          const std::string& name) {
        if (!src_path) throw std::runtime_error("Mock path is null");

        fs::path src(src_path);
        fs::path dest = test_root / subdir / src.filename();

        if (fs::exists(dest)) fs::remove(dest);
        fs::copy_file(src, dest);

        nlohmann::json meta = {
            {"meta", {{"name", name}, {"version", "1.0.0"}}},
            {"integrity", {{"hash", pm->calculate_sha256(dest)}}}
        };
        std::ofstream(dest.string() + ".json") << meta.dump(4);
        return dest;
    }

    /** Convenience: install handler mock directly into test_root/handlers/. */
    fs::path install_handler(const char* src, const std::string& name) {
        return install_mock(src, "handlers", name);
    }

    /** Convenience: install connector mock into test_root/connectors/. */
    fs::path install_connector(const char* src, const std::string& name) {
        return install_mock(src, "connectors", name);
    }

    fs::path test_root;
    std::unique_ptr<gn::PluginManager> pm;
};

// ─── Load tests ───────────────────────────────────────────────────────────────

/**
 * @brief load_plugin() must succeed for a valid handler .so.
 * The handler must be findable by the name declared in get_plugin_name().
 */
TEST_F(PluginManagerTest, LoadHandlerWithCppSdk) {
    auto path = install_handler(MOCK_HANDLER_PATH, "mock_handler");

    auto res = pm->load_plugin(path);
    ASSERT_TRUE(res.has_value()) << "Load error: " << res.error();

    auto h = pm->find_handler_by_name("mock_handler");
    ASSERT_TRUE(h.has_value());
    EXPECT_STREQ((*h)->name, "mock_handler");
}

/**
 * @brief load_plugin() must fail gracefully for a non-existent path.
 */
TEST_F(PluginManagerTest, LoadNonExistentPlugin) {
    auto res = pm->load_plugin(test_root / "handlers" / "ghost.so");
    EXPECT_FALSE(res.has_value());
    EXPECT_FALSE(res.error().empty());
}

/**
 * @brief load_plugin() must reject a .so whose manifest hash does not match.
 */
TEST_F(PluginManagerTest, LoadPluginWithCorruptedManifest) {
    auto dest = install_handler(MOCK_HANDLER_PATH, "corrupt");

    // Overwrite the manifest with a wrong hash
    nlohmann::json bad = {
        {"meta", {{"name", "corrupt"}, {"version", "1.0.0"}}},
        {"integrity", {{"hash", std::string(64, '0')}}}
    };
    std::ofstream(dest.string() + ".json") << bad.dump(4);

    auto res = pm->load_plugin(dest);
    EXPECT_FALSE(res.has_value());
}

/**
 * @brief load_plugin() must reject a .so with no manifest at all.
 */
TEST_F(PluginManagerTest, LoadPluginWithoutManifest) {
    fs::path src(MOCK_HANDLER_PATH);
    fs::path dest = test_root / "handlers" / src.filename();
    fs::copy_file(src, dest);
    // No .json written

    auto res = pm->load_plugin(dest);
    EXPECT_FALSE(res.has_value());
}

// ─── Connector tests ──────────────────────────────────────────────────────────

/**
 * @brief Connector must be findable by scheme returned from get_scheme().
 * The name returned by get_name() must not be the filename stem.
 */
TEST_F(PluginManagerTest, ConnectorSchemeAndName) {
    auto path = install_connector(MOCK_CONNECTOR_PATH, "MockDevice");

    ASSERT_TRUE(pm->load_plugin(path).has_value());

    auto conn = pm->find_connector_by_scheme("mock");
    ASSERT_TRUE(conn.has_value());

    char name_buf[128] = {};
    (*conn)->get_name((*conn)->connector_ctx, name_buf, sizeof(name_buf));
    EXPECT_STREQ(name_buf, "MockDevice");

    char scheme_buf[64] = {};
    (*conn)->get_scheme((*conn)->connector_ctx, scheme_buf, sizeof(scheme_buf));
    EXPECT_STREQ(scheme_buf, "mock");
}

// ─── Directory scan ───────────────────────────────────────────────────────────

/**
 * @brief load_all_plugins() must discover and load all valid .so files
 * placed under handlers/ and connectors/ subdirectories.
 */
TEST_F(PluginManagerTest, FullDirectoryScan) {
    install_handler  (MOCK_HANDLER_PATH,   "auto_handler");
    install_connector(MOCK_CONNECTOR_PATH, "auto_connector");

    pm->load_all_plugins();

    EXPECT_EQ(pm->get_enabled_handler_count(),   1u);
    EXPECT_EQ(pm->get_enabled_connector_count(), 1u);

    EXPECT_TRUE(pm->find_handler_by_name("mock_handler").has_value());
    EXPECT_TRUE(pm->find_connector_by_scheme("mock").has_value());
}

/**
 * @brief load_all_plugins() on an empty directory must not crash or load
 * anything.
 */
TEST_F(PluginManagerTest, ScanEmptyDirectory) {
    pm->load_all_plugins();
    EXPECT_EQ(pm->get_enabled_handler_count(),   0u);
    EXPECT_EQ(pm->get_enabled_connector_count(), 0u);
}

// ─── State management ─────────────────────────────────────────────────────────

/**
 * @brief disable_handler() must hide the handler from find_handler_by_name()
 * without removing it; enable_handler() must restore visibility.
 */
TEST_F(PluginManagerTest, HandlerStateManagement) {
    auto path = install_handler(MOCK_HANDLER_PATH, "mock_handler");
    ASSERT_TRUE(pm->load_plugin(path).has_value());

    // Freshly loaded → enabled
    EXPECT_TRUE(pm->find_handler_by_name("mock_handler").has_value());
    EXPECT_EQ(pm->get_enabled_handler_count(), 1u);

    // Disable
    EXPECT_TRUE(pm->disable_handler("mock_handler"));
    EXPECT_FALSE(pm->find_handler_by_name("mock_handler").has_value());
    EXPECT_EQ(pm->get_enabled_handler_count(), 0u);

    // Re-enable
    EXPECT_TRUE(pm->enable_handler("mock_handler"));
    EXPECT_TRUE(pm->find_handler_by_name("mock_handler").has_value());
    EXPECT_EQ(pm->get_enabled_handler_count(), 1u);

    // Unload completely
    EXPECT_TRUE(pm->unload_handler("mock_handler"));
    EXPECT_FALSE(pm->find_handler_by_name("mock_handler").has_value());
    EXPECT_EQ(pm->get_enabled_handler_count(), 0u);
}

/**
 * @brief Operations on non-existent handler names must return false.
 */
TEST_F(PluginManagerTest, StateOpsOnMissingHandler) {
    EXPECT_FALSE(pm->disable_handler("ghost"));
    EXPECT_FALSE(pm->enable_handler("ghost"));
    EXPECT_FALSE(pm->unload_handler("ghost"));
}

// ─── Active list ──────────────────────────────────────────────────────────────

/**
 * @brief get_active_handlers() must reflect enabled count accurately.
 */
TEST_F(PluginManagerTest, GetActiveHandlersList) {
    install_handler(MOCK_HANDLER_PATH, "mock_handler");
    pm->load_all_plugins();

    auto active = pm->get_active_handlers();
    ASSERT_EQ(active.size(), 1u);
    EXPECT_NE(active[0], nullptr);
    EXPECT_STREQ(active[0]->name, "mock_handler");

    pm->disable_handler("mock_handler");
    EXPECT_EQ(pm->get_active_handlers().size(), 0u);
}

// ─── Double load / idempotency ────────────────────────────────────────────────

/**
 * @brief Loading the same .so twice must not duplicate the handler entry
 * (second load overwrites or returns an error — implementation-defined,
 * but count must remain 1).
 */
TEST_F(PluginManagerTest, DoubleLoadSamePlugin) {
    auto path = install_handler(MOCK_HANDLER_PATH, "mock_handler");
    ASSERT_TRUE(pm->load_plugin(path).has_value());
    // Second load: may succeed (overwrite) or fail — count must be <= 1
    pm->load_plugin(path);
    EXPECT_LE(pm->get_enabled_handler_count(), 1u);
}

// ─── unload_all ───────────────────────────────────────────────────────────────

/**
 * @brief unload_all() must clear every handler and connector.
 */
TEST_F(PluginManagerTest, UnloadAll) {
    install_handler  (MOCK_HANDLER_PATH,   "mock_handler");
    install_connector(MOCK_CONNECTOR_PATH, "mock_connector");
    pm->load_all_plugins();

    ASSERT_GT(pm->get_enabled_handler_count()   + 
              pm->get_enabled_connector_count(), 0u);

    pm->unload_all();

    EXPECT_EQ(pm->get_enabled_handler_count(),   0u);
    EXPECT_EQ(pm->get_enabled_connector_count(), 0u);
    EXPECT_FALSE(pm->find_handler_by_name("mock_handler").has_value());
    EXPECT_FALSE(pm->find_connector_by_scheme("mock").has_value());
}