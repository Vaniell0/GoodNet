#include <sodium.h>
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <boost/asio.hpp>

#include "pm/pluginManager.hpp"
#include "dynlib.hpp"
#include "signals.hpp"
#include "cm/connectionManager.hpp"

using namespace gn;
namespace fs = std::filesystem;

#ifndef MOCK_HANDLER_PATH
#  error "Define MOCK_HANDLER_PATH in CMakeLists.txt"
#endif
#ifndef MOCK_CONNECTOR_PATH
#  error "Define MOCK_CONNECTOR_PATH in CMakeLists.txt"
#endif

// ─── Helpers ──────────────────────────────────────────────────────────────────

static void write_manifest(const fs::path& so_path) {
    std::ifstream f(so_path, std::ios::binary);
    ASSERT_TRUE(f.is_open()) << "Cannot open .so: " << so_path;

    crypto_hash_sha256_state state;
    crypto_hash_sha256_init(&state);
    std::vector<char> buf(64 * 1024);
    while (f) {
        f.read(buf.data(), (std::streamsize)buf.size());
        if (f.gcount() > 0)
            crypto_hash_sha256_update(&state,
                reinterpret_cast<const uint8_t*>(buf.data()),
                (unsigned long long)f.gcount());
    }
    uint8_t hash[32];
    crypto_hash_sha256_final(&state, hash);

    std::string hex;
    for (uint8_t b : hash) {
        char tmp[3]; snprintf(tmp, sizeof(tmp), "%02x", b); hex += tmp;
    }

    nlohmann::json manifest;
    manifest["meta"]["name"]        = "mock";
    manifest["meta"]["version"]     = "1.0.0";
    manifest["meta"]["type"]        = "handler"; // will be overridden by actual plugin type
    manifest["integrity"]["algorithm"] = "sha256";
    manifest["integrity"]["hash"]      = hex;

    std::ofstream mf(so_path.string() + ".json");
    ASSERT_TRUE(mf.is_open());
    mf << manifest.dump(2);
}

// ─── Fixture ──────────────────────────────────────────────────────────────────

class PMTest : public ::testing::Test {
protected:
    boost::asio::io_context  ioc_;
    SignalBus                bus_{ioc_};
    fs::path                 id_dir_ = [] {
        auto p = fs::temp_directory_path() / "gn_pm_test_id";
        fs::create_directories(p); return p;
    }();
    NodeIdentity             identity_ = NodeIdentity::load_or_generate(id_dir_);
    ConnectionManager        cm_{bus_, identity_};
    
    host_api_t               api_c_{}; 
    PluginManager            pm_;
    PMTest() : pm_(&api_c_) {}

    void SetUp() override {
        if (sodium_init() < 0) FAIL() << "sodium_init failed";
        cm_.fill_host_api(&api_c_);
    }

    void TearDown() override {
        pm_.unload_all();
        fs::remove_all(id_dir_);
    }

    fs::path handler_path()   { fs::path p(MOCK_HANDLER_PATH);   write_manifest(p); return p; }
    fs::path connector_path() { fs::path p(MOCK_CONNECTOR_PATH); write_manifest(p); return p; }
};

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 1 & 2: load_plugin
// ═══════════════════════════════════════════════════════════════════════════════

TEST_F(PMTest, LoadHandler_Success) {
    auto p = handler_path();
    auto result = pm_.load_plugin(p);
    EXPECT_TRUE(result.has_value()) << (result.has_value() ? "" : result.error());
    EXPECT_EQ(pm_.get_enabled_handler_count(), 1u); // Updated
}

TEST_F(PMTest, LoadConnector_Success) {
    auto p = connector_path();
    auto result = pm_.load_plugin(p);
    EXPECT_TRUE(result.has_value()) << (result.has_value() ? "" : result.error());
    EXPECT_EQ(pm_.get_enabled_connector_count(), 1u); // Updated
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 3: Query methods
// ═══════════════════════════════════════════════════════════════════════════════

TEST_F(PMTest, FindHandlerByName_Found) {
    pm_.load_plugin(handler_path());
    auto info = pm_.find_handler_by_name("mock_handler");
    ASSERT_TRUE(info.has_value());
    ASSERT_NE(*info, nullptr);
    EXPECT_STREQ((*info)->name, "mock_handler");
}

TEST_F(PMTest, FindConnectorByScheme_Found) {
    pm_.load_plugin(connector_path());
    auto info = pm_.find_connector_by_scheme("mock");
    ASSERT_TRUE(info.has_value());
    ASSERT_NE(*info, nullptr);
}

TEST_F(PMTest, ListHandlerNames) {
    pm_.load_plugin(handler_path());
    auto handlers = pm_.get_active_handlers(); 
    ASSERT_FALSE(handlers.empty());
    EXPECT_STREQ(handlers[0]->name, "mock_handler");
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 6: DynLib direct tests
// ═══════════════════════════════════════════════════════════════════════════════

TEST(DynLibTest, OpenAndSymbol_ValidLib) {
    auto result = gn::DynLib::open("libsodium.so");
    if (!result.has_value()) GTEST_SKIP() << "libsodium.so not found";
    
    using version_fn = const char* (*)();
    auto sym = result.value().symbol<version_fn>("sodium_version_string");
    EXPECT_TRUE(sym.has_value());
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 7: IConnector operations
// ═══════════════════════════════════════════════════════════════════════════════

TEST_F(PMTest, MockConnector_SendNoOp) {
    pm_.load_plugin(connector_path());
    auto info = pm_.find_connector_by_scheme("mock");
    ASSERT_TRUE(info.has_value());

    uint8_t data[] = {1, 2, 3};
    // info contains connector_ops_t* directly
    int ret = (*info)->send_to((*info)->connector_ctx, 42, data, sizeof(data));
    EXPECT_EQ(ret, 0);
}

TEST_F(PMTest, MockConnector_ConnectReturnsError) {
    pm_.load_plugin(connector_path());
    auto info = pm_.find_connector_by_scheme("mock");
    ASSERT_TRUE(info.has_value());

    int ret = (*info)->connect((*info)->connector_ctx, "mock://10.0.0.1:1234");
    EXPECT_NE(ret, 0); 
}

TEST_F(PMTest, MockConnector_ListenSucceeds) {
    pm_.load_plugin(connector_path());
    auto info = pm_.find_connector_by_scheme("mock");
    ASSERT_TRUE(info.has_value());

    int ret = (*info)->listen((*info)->connector_ctx, "0.0.0.0", 9999);
    EXPECT_EQ(ret, 0);
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 8: Thread safety
// ═══════════════════════════════════════════════════════════════════════════════

TEST_F(PMTest, ConcurrentQueryWhileLoading_NoDataRace) {
    auto p = handler_path();
    std::atomic<bool> stop{false};
    
    std::thread reader([this, &stop]() {
        while(!stop) {
            (void)this->pm_.get_enabled_handler_count();
            (void)this->pm_.find_handler_by_name("mock");
            std::this_thread::yield();
        }
    });

    auto res = pm_.load_plugin(p);
    EXPECT_TRUE(res.has_value());
    
    stop = true;
    reader.join();
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 9: Enable/Disable, Unload, Verify
// ═══════════════════════════════════════════════════════════════════════════════

TEST_F(PMTest, EnableDisable_StateToggle) {
    pm_.load_plugin(handler_path());
    EXPECT_EQ(pm_.get_enabled_handler_count(), 1u);

    EXPECT_TRUE(pm_.disable_handler("mock_handler"));
    EXPECT_EQ(pm_.get_enabled_handler_count(), 0u);

    // find_handler_by_name returns nullopt for disabled handler
    EXPECT_FALSE(pm_.find_handler_by_name("mock_handler").has_value());

    EXPECT_TRUE(pm_.enable_handler("mock_handler"));
    EXPECT_EQ(pm_.get_enabled_handler_count(), 1u);
    EXPECT_TRUE(pm_.find_handler_by_name("mock_handler").has_value());
}

TEST_F(PMTest, UnloadHandler_Removes) {
    pm_.load_plugin(handler_path());
    EXPECT_EQ(pm_.get_enabled_handler_count(), 1u);

    EXPECT_TRUE(pm_.unload_handler("mock_handler"));
    EXPECT_EQ(pm_.get_enabled_handler_count(), 0u);

    // After unload, find should return nullopt
    EXPECT_FALSE(pm_.find_handler_by_name("mock_handler").has_value());
}

TEST_F(PMTest, VerifyMetadata_WrongHash) {
    auto p = handler_path();
    // Corrupt the manifest hash
    auto manifest_path = p.string() + ".json";
    nlohmann::json manifest;
    {
        std::ifstream f(manifest_path);
        manifest = nlohmann::json::parse(f);
    }
    manifest["integrity"]["hash"] = "0000000000000000000000000000000000000000000000000000000000000000";
    {
        std::ofstream f(manifest_path);
        f << manifest.dump(2);
    }

    auto result = pm_.load_plugin(p);
    EXPECT_FALSE(result.has_value());
}

TEST_F(PMTest, MissingManifest_Reject) {
    auto p = handler_path();
    // Remove the manifest file
    fs::remove(p.string() + ".json");

    auto result = pm_.load_plugin(p);
    EXPECT_FALSE(result.has_value());
}

TEST_F(PMTest, EnableDisable_NonExistent_ReturnsFalse) {
    EXPECT_FALSE(pm_.enable_handler("nonexistent"));
    EXPECT_FALSE(pm_.disable_handler("nonexistent"));
    EXPECT_FALSE(pm_.unload_handler("nonexistent"));
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 10: Additional pm/core.cpp coverage
// ═══════════════════════════════════════════════════════════════════════════════

TEST_F(PMTest, LoadPlugin_NonExistentPath) {
    auto result = pm_.load_plugin("/nonexistent/path/plugin.so");
    EXPECT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("not found"), std::string::npos);
}

TEST_F(PMTest, LoadHandler_Duplicate_Rejected) {
    auto p = handler_path();
    auto r1 = pm_.load_plugin(p);
    EXPECT_TRUE(r1.has_value()) << (r1.has_value() ? "" : r1.error());

    auto r2 = pm_.load_plugin(p);
    EXPECT_FALSE(r2.has_value());
    EXPECT_NE(r2.error().find("already loaded"), std::string::npos);
}

TEST_F(PMTest, LoadConnector_Duplicate_Rejected) {
    auto p = connector_path();
    auto r1 = pm_.load_plugin(p);
    EXPECT_TRUE(r1.has_value()) << (r1.has_value() ? "" : r1.error());

    auto r2 = pm_.load_plugin(p);
    EXPECT_FALSE(r2.has_value());
    EXPECT_NE(r2.error().find("already loaded"), std::string::npos);
}

TEST_F(PMTest, UnloadAll_ClearsEverything) {
    pm_.load_plugin(handler_path());
    pm_.load_plugin(connector_path());
    EXPECT_EQ(pm_.get_enabled_handler_count(), 1u);
    EXPECT_EQ(pm_.get_enabled_connector_count(), 1u);

    pm_.unload_all();
    EXPECT_EQ(pm_.get_enabled_handler_count(), 0u);
    EXPECT_EQ(pm_.get_enabled_connector_count(), 0u);
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 11: load_all_plugins
// ═══════════════════════════════════════════════════════════════════════════════

TEST(PMLoadAllTest, LoadAll_EmptyDir) {
    auto dir = fs::temp_directory_path() / "gn_pm_loadall_empty";
    fs::create_directories(dir);

    host_api_t api{};
    PluginManager pm(&api, dir);
    EXPECT_NO_THROW(pm.load_all_plugins());
    EXPECT_EQ(pm.get_enabled_handler_count(), 0u);

    fs::remove_all(dir);
}

TEST(PMLoadAllTest, LoadAll_EmptyBasePath) {
    host_api_t api{};
    PluginManager pm(&api);
    EXPECT_NO_THROW(pm.load_all_plugins());
}

TEST(PMLoadAllTest, LoadAll_WithPlugins) {
    auto dir = fs::temp_directory_path() / "gn_pm_loadall";
    fs::create_directories(dir);

    // Копируем mock handler в директорию
    fs::path handler_src(MOCK_HANDLER_PATH);
    fs::path handler_dst = dir / handler_src.filename();
    fs::copy_file(handler_src, handler_dst, fs::copy_options::overwrite_existing);
    write_manifest(handler_dst);

    boost::asio::io_context ioc;
    SignalBus bus(ioc);
    auto id_dir = fs::temp_directory_path() / "gn_pm_loadall_id";
    fs::create_directories(id_dir);
    auto identity = NodeIdentity::load_or_generate(id_dir);
    ConnectionManager cm(bus, identity);

    host_api_t api{};
    cm.fill_host_api(&api);
    PluginManager pm(&api, dir);
    pm.load_all_plugins();

    EXPECT_GE(pm.get_enabled_handler_count(), 1u);

    pm.unload_all();
    fs::remove_all(dir);
    fs::remove_all(id_dir);
}

TEST(PMLoadAllTest, LoadAll_NonPluginSoIgnored) {
    auto dir = fs::temp_directory_path() / "gn_pm_loadall_nonso";
    fs::create_directories(dir);

    // Создаём .txt файл — должен быть проигнорирован
    std::ofstream(dir / "not_a_plugin.txt") << "hello";

    host_api_t api{};
    PluginManager pm(&api, dir);
    EXPECT_NO_THROW(pm.load_all_plugins());
    EXPECT_EQ(pm.get_enabled_handler_count(), 0u);

    fs::remove_all(dir);
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 12: Connector queries & unload
// ═══════════════════════════════════════════════════════════════════════════════

TEST_F(PMTest, UnloadConnector_Removes) {
    pm_.load_plugin(connector_path());
    EXPECT_EQ(pm_.get_enabled_connector_count(), 1u);

    EXPECT_TRUE(pm_.unload_connector("mock"));
    EXPECT_EQ(pm_.get_enabled_connector_count(), 0u);
    EXPECT_FALSE(pm_.find_connector_by_scheme("mock").has_value());
}

TEST_F(PMTest, UnloadConnector_NonExistent) {
    EXPECT_FALSE(pm_.unload_connector("nonexistent"));
}

TEST_F(PMTest, GetActiveConnectors) {
    pm_.load_plugin(connector_path());
    auto connectors = pm_.get_active_connectors();
    ASSERT_FALSE(connectors.empty());
    EXPECT_NE(connectors[0], nullptr);
}

TEST_F(PMTest, GetEnabledHandlerNames) {
    pm_.load_plugin(handler_path());
    auto names = pm_.get_enabled_handler_names();
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names[0], "mock_handler");
}

TEST_F(PMTest, ListPlugins_NoCrash) {
    pm_.load_plugin(handler_path());
    pm_.load_plugin(connector_path());
    EXPECT_NO_THROW(pm_.list_plugins());
}

TEST_F(PMTest, CalculateSha256) {
    auto hash = PluginManager::calculate_sha256(handler_path());
    EXPECT_EQ(hash.size(), 64u);
    for (char c : hash) {
        EXPECT_TRUE(std::isxdigit(static_cast<unsigned char>(c)))
            << "Non-hex char: " << c;
    }
}