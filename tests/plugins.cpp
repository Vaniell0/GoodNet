#include <sodium.h>
#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <boost/asio.hpp>

#include "pluginManager.hpp"
#include "signals.hpp"
#include "connectionManager.hpp"

using namespace gn;
namespace fs = std::filesystem;
using json = nlohmann::json;

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