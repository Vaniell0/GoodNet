#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <filesystem>
#include <fstream>
#include <sodium.h>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <nlohmann/json.hpp>

#include "core/pluginManager.hpp"
#include "sdk/types.h"
#include "sdk/plugin.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

// ─── Фикстура ────────────────────────────────────────────────────────────────

class PluginManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (sodium_init() < 0)
            GTEST_SKIP() << "libsodium init failed";

        test_root = fs::temp_directory_path() /
                    fmt::format("goodnet_pm_test_{}", ::getpid());
        fs::create_directories(test_root / "handlers");
        fs::create_directories(test_root / "connectors");

        // Пустой api
        static host_api_t api{};
        pm = std::make_unique<gn::PluginManager>(&api, test_root);
    }

    void TearDown() override {
        pm.reset();
        if (fs::exists(test_root)) {
            fs::remove_all(test_root);
        }
    }

    /**
     * @brief Установить мок-плагин
     * Использует статический метод PluginManager::calculate_sha256 для генерации подписи
     */
    fs::path install_mock(const char* src_path,
                          const std::string& internal_name,
                          const fs::path&    subdir = "") {
        if (!src_path) throw std::runtime_error("mock path is null");

        fs::path src(src_path);
        fs::path dest_dir = subdir.empty() ? test_root : test_root / subdir;
        fs::path dest     = dest_dir / src.filename();

        fs::create_directories(dest_dir);
        if (fs::exists(dest)) fs::remove(dest);
        fs::copy_file(src, dest);

        std::string actual_hash = gn::PluginManager::calculate_sha256(dest);

        // Манифест: <plugin>.so.json
        // Структура должна совпадать с verify_metadata
        json manifest = {
            {"meta", {
                {"name", internal_name},
                {"version", "1.0.0"}
            }},
            {"integrity", {
                {"hash", actual_hash}
            }}
        };

        std::string json_path = dest.string() + ".json";
        std::ofstream ofs(json_path);
        ofs << manifest.dump(4);
        ofs.close();

        return dest;
    }

    fs::path test_root;
    std::unique_ptr<gn::PluginManager> pm;
};

// ─── Загрузка хендлера ────────────────────────────────────────────────────────

TEST_F(PluginManagerTest, LoadHandlerSuccess) {
    auto path = install_mock(MOCK_HANDLER_PATH, "mock_handler");
    auto res  = pm->load_plugin(path);

    ASSERT_TRUE(res.has_value()) << "Load error: " << res.error();
    EXPECT_EQ(pm->get_enabled_handler_count(), 1u);
}

TEST_F(PluginManagerTest, LoadedHandlerHasCorrectName) {
    auto path = install_mock(MOCK_HANDLER_PATH, "mock_handler");
    ASSERT_TRUE(pm->load_plugin(path).has_value());

    auto h = pm->find_handler_by_name("mock_handler");
    ASSERT_TRUE(h.has_value());
    ASSERT_NE((*h)->name, nullptr);
    EXPECT_STREQ((*h)->name, "mock_handler");
}

TEST_F(PluginManagerTest, LoadedHandlerHasHandleMessageCallback) {
    auto path = install_mock(MOCK_HANDLER_PATH, "mock_handler");
    ASSERT_TRUE(pm->load_plugin(path).has_value());

    auto h = pm->find_handler_by_name("mock_handler");
    ASSERT_TRUE(h.has_value());
    EXPECT_NE((*h)->handle_message, nullptr);
}

TEST_F(PluginManagerTest, MissingManifestFails) {
    fs::path src(MOCK_HANDLER_PATH);
    fs::path dest = test_root / src.filename();
    fs::copy_file(src, dest);

    auto res = pm->load_plugin(dest);
    EXPECT_FALSE(res.has_value());
    EXPECT_THAT(res.error(), ::testing::HasSubstr("manifest"));
}

TEST_F(PluginManagerTest, TamperedHashFails) {
    auto path = install_mock(MOCK_HANDLER_PATH, "tampered");

    // Читаем только что созданный манифест и портим в нем хеш
    std::string json_path = path.string() + ".json";
    json data;
    {
        std::ifstream ifs(json_path);
        data = json::parse(ifs);
    }

    data["integrity"]["hash"] = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"; // Empty file hash

    {
        std::ofstream ofs(json_path);
        ofs << data.dump(4);
    }

    auto res = pm->load_plugin(path);
    EXPECT_FALSE(res.has_value());
    // Теперь сообщение об ошибке должно приходить именно от verify_metadata
    EXPECT_THAT(res.error(), ::testing::HasSubstr("Hash mismatch"));
}

TEST_F(PluginManagerTest, NonExistentPathFails) {
    auto res = pm->load_plugin("/nonexistent/path/plugin.so");
    EXPECT_FALSE(res.has_value());
}

TEST_F(PluginManagerTest, DuplicateHandlerFails) {
    auto path = install_mock(MOCK_HANDLER_PATH, "mock_handler");
    ASSERT_TRUE(pm->load_plugin(path).has_value());

    // Второй раз тот же .so (scheme/name уже занято)
    auto res = pm->load_plugin(path);
    EXPECT_FALSE(res.has_value());
}

// ─── Загрузка коннектора ─────────────────────────────────────────────────────

TEST_F(PluginManagerTest, LoadConnectorSuccess) {
    auto path = install_mock(MOCK_CONNECTOR_PATH, "mock_connector");
    auto res  = pm->load_plugin(path);

    ASSERT_TRUE(res.has_value()) << "Load error: " << res.error();
    EXPECT_EQ(pm->get_enabled_connector_count(), 1u);
}

TEST_F(PluginManagerTest, ConnectorSchemeIsCorrect) {
    auto path = install_mock(MOCK_CONNECTOR_PATH, "mock_connector");
    ASSERT_TRUE(pm->load_plugin(path).has_value());

    auto opt = pm->find_connector_by_scheme("mock");
    ASSERT_TRUE(opt.has_value());
    EXPECT_NE(*opt, nullptr);
}

TEST_F(PluginManagerTest, ConnectorNameFromGetName) {
    auto path = install_mock(MOCK_CONNECTOR_PATH, "mock_connector");
    ASSERT_TRUE(pm->load_plugin(path).has_value());

    auto opt = pm->find_connector_by_scheme("mock");
    ASSERT_TRUE(opt.has_value());

    char buf[128] = {};
    (*opt)->get_name((*opt)->connector_ctx, buf, sizeof(buf));
    EXPECT_STREQ(buf, "MockConnector");
}

TEST_F(PluginManagerTest, ConnectorHasRequiredCallbacks) {
    auto path = install_mock(MOCK_CONNECTOR_PATH, "mock_connector");
    ASSERT_TRUE(pm->load_plugin(path).has_value());

    auto opt = pm->find_connector_by_scheme("mock");
    ASSERT_TRUE(opt.has_value());
    auto* ops = *opt;

    EXPECT_NE(ops->connect,    nullptr);
    EXPECT_NE(ops->listen,     nullptr);
    EXPECT_NE(ops->send_to,    nullptr);
    EXPECT_NE(ops->close,      nullptr);
    EXPECT_NE(ops->get_scheme, nullptr);
    EXPECT_NE(ops->get_name,   nullptr);
    EXPECT_NE(ops->shutdown,   nullptr);
}

TEST_F(PluginManagerTest, UnknownSchemereturnsNullopt) {
    EXPECT_FALSE(pm->find_connector_by_scheme("tcp").has_value());
    EXPECT_FALSE(pm->find_connector_by_scheme("").has_value());
}

// ─── Управление состоянием ────────────────────────────────────────────────────

TEST_F(PluginManagerTest, HandlerEnabledByDefault) {
    auto path = install_mock(MOCK_HANDLER_PATH, "mock_handler");
    ASSERT_TRUE(pm->load_plugin(path).has_value());

    // enabled → find_handler_by_name возвращает Some
    EXPECT_TRUE(pm->find_handler_by_name("mock_handler").has_value());
}

TEST_F(PluginManagerTest, DisabledHandlerNotFound) {
    auto path = install_mock(MOCK_HANDLER_PATH, "mock_handler");
    ASSERT_TRUE(pm->load_plugin(path).has_value());

    EXPECT_TRUE(pm->disable_handler("mock_handler"));
    EXPECT_FALSE(pm->find_handler_by_name("mock_handler").has_value());
    EXPECT_EQ(pm->get_enabled_handler_count(), 0u);
}

TEST_F(PluginManagerTest, ReenableHandlerRestoresAccess) {
    auto path = install_mock(MOCK_HANDLER_PATH, "mock_handler");
    ASSERT_TRUE(pm->load_plugin(path).has_value());

    pm->disable_handler("mock_handler");
    EXPECT_TRUE(pm->enable_handler("mock_handler"));
    EXPECT_TRUE(pm->find_handler_by_name("mock_handler").has_value());
    EXPECT_EQ(pm->get_enabled_handler_count(), 1u);
}

TEST_F(PluginManagerTest, UnloadHandlerRemovesPlugin) {
    auto path = install_mock(MOCK_HANDLER_PATH, "mock_handler");
    ASSERT_TRUE(pm->load_plugin(path).has_value());

    EXPECT_TRUE(pm->unload_handler("mock_handler"));
    EXPECT_FALSE(pm->find_handler_by_name("mock_handler").has_value());
    EXPECT_EQ(pm->get_enabled_handler_count(), 0u);
}

TEST_F(PluginManagerTest, UnloadNonexistentHandlerReturnsFalse) {
    EXPECT_FALSE(pm->unload_handler("does_not_exist"));
}

TEST_F(PluginManagerTest, DisableNonexistentHandlerReturnsFalse) {
    EXPECT_FALSE(pm->disable_handler("does_not_exist"));
}

// ─── get_active_handlers ──────────────────────────────────────────────────────

TEST_F(PluginManagerTest, GetActiveHandlersReflectsEnabledState) {
    auto path = install_mock(MOCK_HANDLER_PATH, "mock_handler");
    ASSERT_TRUE(pm->load_plugin(path).has_value());

    EXPECT_EQ(pm->get_active_handlers().size(), 1u);

    pm->disable_handler("mock_handler");
    EXPECT_EQ(pm->get_active_handlers().size(), 0u);

    pm->enable_handler("mock_handler");
    EXPECT_EQ(pm->get_active_handlers().size(), 1u);
}

TEST_F(PluginManagerTest, GetActiveHandlersReturnsValidPointers) {
    auto path = install_mock(MOCK_HANDLER_PATH, "mock_handler");
    ASSERT_TRUE(pm->load_plugin(path).has_value());

    auto active = pm->get_active_handlers();
    ASSERT_EQ(active.size(), 1u);
    EXPECT_NE(active[0], nullptr);
    EXPECT_NE(active[0]->handle_message, nullptr);
}

// ─── Автосканирование директорий ─────────────────────────────────────────────

TEST_F(PluginManagerTest, LoadAllPluginsScansHandlersDir) {
    install_mock(MOCK_HANDLER_PATH, "auto_handler", "handlers");

    pm->load_all_plugins();

    // handler name = из handler->name, установленного в MockHandler::get_plugin_name()
    EXPECT_EQ(pm->get_enabled_handler_count(), 1u);
}

TEST_F(PluginManagerTest, LoadAllPluginsScansConnectorsDir) {
    install_mock(MOCK_CONNECTOR_PATH, "auto_connector", "connectors");

    pm->load_all_plugins();

    EXPECT_EQ(pm->get_enabled_connector_count(), 1u);
    EXPECT_TRUE(pm->find_connector_by_scheme("mock").has_value());
}

TEST_F(PluginManagerTest, LoadAllPluginsFromBothDirs) {
    install_mock(MOCK_HANDLER_PATH,   "auto_h", "handlers");
    install_mock(MOCK_CONNECTOR_PATH, "auto_c", "connectors");

    pm->load_all_plugins();

    EXPECT_EQ(pm->get_enabled_handler_count(),   1u);
    EXPECT_EQ(pm->get_enabled_connector_count(), 1u);
}

TEST_F(PluginManagerTest, EmptyDirLoadsNothing) {
    // handlers/ и connectors/ пустые — load_all_plugins не должен упасть
    ASSERT_NO_THROW(pm->load_all_plugins());
    EXPECT_EQ(pm->get_enabled_handler_count(), 0u);
}

// ─── unload_all ───────────────────────────────────────────────────────────────

TEST_F(PluginManagerTest, UnloadAllClearsEverything) {
    install_mock(MOCK_HANDLER_PATH,   "h", "handlers");
    install_mock(MOCK_CONNECTOR_PATH, "c", "connectors");
    pm->load_all_plugins();

    ASSERT_GT(pm->get_enabled_handler_count()   + 
              pm->get_enabled_connector_count(), 0u);

    pm->unload_all();

    EXPECT_EQ(pm->get_enabled_handler_count(),   0u);
    EXPECT_EQ(pm->get_enabled_connector_count(), 0u);
}

// ─── Потокобезопасность ───────────────────────────────────────────────────────

TEST_F(PluginManagerTest, ConcurrentFindIsThreadSafe) {
    auto path = install_mock(MOCK_HANDLER_PATH, "mock_handler");
    ASSERT_TRUE(pm->load_plugin(path).has_value());

    // 20 потоков параллельно читают
    std::vector<std::thread> threads;
    threads.reserve(20);
    for (int i = 0; i < 20; ++i)
        threads.emplace_back([&] {
            for (int j = 0; j < 100; ++j)
                pm->find_handler_by_name("mock_handler");
        });
    for (auto& t : threads) t.join();
    // Не должно быть data race (проверяется через TSAN)
    SUCCEED();
}

TEST_F(PluginManagerTest, ConcurrentEnableDisableIsThreadSafe) {
    auto path = install_mock(MOCK_HANDLER_PATH, "mock_handler");
    ASSERT_TRUE(pm->load_plugin(path).has_value());

    std::vector<std::thread> threads;
    threads.reserve(10);
    for (int i = 0; i < 10; ++i)
        threads.emplace_back([&, i] {
            if (i % 2 == 0) pm->disable_handler("mock_handler");
            else             pm->enable_handler ("mock_handler");
        });
    for (auto& t : threads) t.join();
    SUCCEED();
}
