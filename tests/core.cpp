#include <gtest/gtest.h>
#include <core.hpp>
#include "core.h"
#include "config.hpp"
#include "version.hpp"
#include "test_helpers.hpp"

#include <cstring>
#include <string>

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 1: Core C++ lifecycle
// ═══════════════════════════════════════════════════════════════════════════════

TEST(CoreTest, FastStartup) {
    Config config(true);
    config.core.io_threads   = 1;
    config.plugins.auto_load = false;

    gn::Core core(&config);
    core.run_async();
    EXPECT_TRUE(core.is_running());
    core.stop();
}

TEST(CoreTest, MultiInstance) {
    Config config(true);
    config.core.io_threads   = 1;
    config.plugins.auto_load = false;

    gn::Core first(&config);
    EXPECT_NO_THROW({
        gn::Core second(&config);
    });
}

TEST(CoreTest, SequentialLifecycle) {
    Config config(true);
    config.core.io_threads   = 1;
    config.plugins.auto_load = false;

    // First instance: create, run, stop, destroy.
    {
        gn::Core core(&config);
        core.run_async();
        core.stop();
    }

    // Second instance: must not throw.
    {
        gn::Core core(&config);
        core.run_async();
        core.stop();
    }
}

TEST(CoreTest, MultipleRunAsyncStopCycles) {
    Config config(true);
    config.core.io_threads   = 1;
    config.plugins.auto_load = false;

    gn::Core core(&config);
    for (int i = 0; i < 3; ++i) {
        core.run_async();
        EXPECT_TRUE(core.is_running());
        core.stop();
        EXPECT_FALSE(core.is_running());
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 2: C API
// ═══════════════════════════════════════════════════════════════════════════════

TEST(CapiTest, LifecycleSanity) {
    auto dir = tmp_dir("capi");
    auto dir_str = dir.string();
    gn_config_t cfg{};
    cfg.config_dir = dir_str.c_str();
    cfg.log_level = "off";
    cfg.listen_port = 0;

    gn_core_t* core = gn_core_create(&cfg);
    ASSERT_NE(core, nullptr);

    // Verify public key marshaling.
    char buf[65];
    size_t len = gn_core_get_user_pubkey(core, buf, sizeof(buf));
    EXPECT_GT(len, 0u);
    EXPECT_EQ(strlen(buf), len);

    // Run/stop lifecycle.
    gn_core_run_async(core, 1);
    gn_core_stop(core);

    gn_core_destroy(core);
    fs::remove_all(dir);
}

TEST(CapiTest, StatsInitiallyZero) {
    auto dir = tmp_dir("capi_stats");
    auto dir_str = dir.string();
    gn_config_t cfg{};
    cfg.config_dir = dir_str.c_str();
    cfg.log_level = "off";
    cfg.listen_port = 0;

    gn_core_t* core = gn_core_create(&cfg);
    ASSERT_NE(core, nullptr);

    gn_stats_t st{};
    gn_core_get_stats(core, &st);
    EXPECT_EQ(st.rx_bytes, 0u);
    EXPECT_EQ(st.tx_bytes, 0u);
    EXPECT_EQ(st.rx_packets, 0u);
    EXPECT_EQ(st.auth_ok, 0u);
    EXPECT_EQ(st.decrypt_fail, 0u);

    gn_core_destroy(core);
    fs::remove_all(dir);
}

TEST(CapiTest, SubscribeUnsubscribe) {
    auto dir = tmp_dir("capi_sub");
    auto dir_str = dir.string();
    gn_config_t cfg{};
    cfg.config_dir = dir_str.c_str();
    cfg.log_level = "off";
    cfg.listen_port = 0;

    gn_core_t* core = gn_core_create(&cfg);
    ASSERT_NE(core, nullptr);

    // Subscribe to MSG_TYPE_CHAT.
    auto cb = [](uint32_t, const void*, size_t, void*) -> propagation_t {
        return PROPAGATION_CONTINUE;
    };

    uint64_t sub = gn_core_subscribe(core, MSG_TYPE_CHAT, cb, nullptr);
    EXPECT_GT(sub, 0u);

    // Unsubscribe should not crash.
    EXPECT_NO_THROW(gn_core_unsubscribe(core, sub));

    gn_core_destroy(core);
    fs::remove_all(dir);
}

TEST(CapiTest, NullCoreSafety) {
    // All C API functions should handle nullptr gracefully.
    EXPECT_NO_THROW(gn_core_run(nullptr));
    EXPECT_NO_THROW(gn_core_run_async(nullptr, 1));
    EXPECT_NO_THROW(gn_core_stop(nullptr));
    EXPECT_NO_THROW(gn_core_destroy(nullptr));
    EXPECT_NO_THROW(gn_core_send(nullptr, "test", 0, nullptr, 0));
    EXPECT_NO_THROW(gn_core_broadcast(nullptr, 0, nullptr, 0));

    gn_stats_t st{};
    EXPECT_NO_THROW(gn_core_get_stats(nullptr, &st));

    char buf[65];
    EXPECT_EQ(gn_core_get_user_pubkey(nullptr, buf, sizeof(buf)), 0u);
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 3: Core → CM delegation
// ═══════════════════════════════════════════════════════════════════════════════

TEST(CoreTest, CoreSend_DoesNotCrash) {
    Config config(true);
    config.core.io_threads   = 1;
    config.plugins.auto_load = false;

    gn::Core core(&config);
    core.run_async();

    // send to unknown URI — message queued for pending delivery
    bool ok = core.send("tcp://10.0.0.99:9999", MSG_TYPE_CHAT,
                        std::string_view("hello", 5));
    EXPECT_TRUE(ok);

    core.stop();
}

TEST(CoreTest, CoreBroadcast_DoesNotCrash) {
    Config config(true);
    config.core.io_threads   = 1;
    config.plugins.auto_load = false;

    gn::Core core(&config);
    core.run_async();

    uint8_t data[] = {1, 2, 3};
    EXPECT_NO_THROW(core.broadcast(MSG_TYPE_CHAT, std::span{data}));

    core.stop();
}

TEST(CoreTest, ActiveConnIds_EmptyInitially) {
    Config config(true);
    config.core.io_threads   = 1;
    config.plugins.auto_load = false;

    gn::Core core(&config);
    auto ids = core.active_conn_ids();
    EXPECT_TRUE(ids.empty());
}

TEST(CoreTest, StatsSnapshot_Valid) {
    Config config(true);
    config.core.io_threads   = 1;
    config.plugins.auto_load = false;

    gn::Core core(&config);
    auto snap = core.stats_snapshot();
    EXPECT_EQ(snap.rx_bytes, 0u);
    EXPECT_EQ(snap.connections, 0u);
}

TEST(CoreTest, ConnectionCount_Zero) {
    Config config(true);
    config.core.io_threads   = 1;
    config.plugins.auto_load = false;

    gn::Core core(&config);
    EXPECT_EQ(core.connection_count(), 0u);
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 4: Additional C API tests
// ═══════════════════════════════════════════════════════════════════════════════

TEST(CapiTest, Disconnect_NoCrash) {
    auto dir = tmp_dir("capi_disc");
    auto dir_str = dir.string();
    gn_config_t cfg{};
    cfg.config_dir = dir_str.c_str();
    cfg.log_level = "off";
    cfg.listen_port = 0;

    gn_core_t* core = gn_core_create(&cfg);
    ASSERT_NE(core, nullptr);

    // Disconnect with invalid conn_id — should not crash
    EXPECT_NO_THROW(gn_core_disconnect(core, 0));
    EXPECT_NO_THROW(gn_core_disconnect(core, 999));

    gn_core_destroy(core);
    fs::remove_all(dir);
}

TEST(CapiTest, Rekey_InvalidConn) {
    auto dir = tmp_dir("capi_rekey");
    auto dir_str = dir.string();
    gn_config_t cfg{};
    cfg.config_dir = dir_str.c_str();
    cfg.log_level = "off";
    cfg.listen_port = 0;

    gn_core_t* core = gn_core_create(&cfg);
    ASSERT_NE(core, nullptr);

    // Rekey on non-existent connection — should return error
    int result = gn_core_rekey(core, 0);
    EXPECT_NE(result, 0);

    gn_core_destroy(core);
    fs::remove_all(dir);
}

TEST(CapiTest, ReloadConfig_NoCrash) {
    auto dir = tmp_dir("capi_reload");
    auto dir_str = dir.string();
    gn_config_t cfg{};
    cfg.config_dir = dir_str.c_str();
    cfg.log_level = "off";
    cfg.listen_port = 0;

    gn_core_t* core = gn_core_create(&cfg);
    ASSERT_NE(core, nullptr);

    EXPECT_NO_THROW(gn_core_reload_config(core));

    gn_core_destroy(core);
    fs::remove_all(dir);
}

TEST(CapiTest, GetUserPubkey_Returns64Hex) {
    auto dir = tmp_dir("capi_pubkey");
    auto dir_str = dir.string();
    gn_config_t cfg{};
    cfg.config_dir = dir_str.c_str();
    cfg.log_level = "off";
    cfg.listen_port = 0;

    gn_core_t* core = gn_core_create(&cfg);
    ASSERT_NE(core, nullptr);

    char buf[128]{};
    size_t len = gn_core_get_user_pubkey(core, buf, sizeof(buf));
    EXPECT_EQ(len, 64u);
    EXPECT_EQ(std::strlen(buf), 64u);

    // All characters should be valid hex
    for (size_t i = 0; i < len; ++i) {
        EXPECT_TRUE(std::isxdigit(static_cast<unsigned char>(buf[i])))
            << "Non-hex char at position " << i << ": " << buf[i];
    }

    gn_core_destroy(core);
    fs::remove_all(dir);
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 5: Core C++ API coverage
// ═══════════════════════════════════════════════════════════════════════════════

TEST(CoreTest, UserAndDevicePubkeyHex) {
    Config config(true);
    config.plugins.auto_load = false;

    gn::Core core(&config);
    auto user_hex = core.user_pubkey_hex();
    auto dev_hex  = core.device_pubkey_hex();

    EXPECT_EQ(user_hex.size(), 64u);
    EXPECT_EQ(dev_hex.size(), 64u);
    EXPECT_NE(user_hex, dev_hex);
}

TEST(CoreTest, PeerPubkey_NoConnection) {
    Config config(true);
    config.plugins.auto_load = false;

    gn::Core core(&config);
    auto pk = core.peer_pubkey(999);
    EXPECT_TRUE(pk.empty());
}

TEST(CoreTest, PeerPubkeyHex_NoConnection) {
    Config config(true);
    config.plugins.auto_load = false;

    gn::Core core(&config);
    auto hex = core.peer_pubkey_hex(999);
    EXPECT_TRUE(hex.empty());
}

TEST(CoreTest, PeerEndpoint_NoConnection) {
    Config config(true);
    config.plugins.auto_load = false;

    gn::Core core(&config);
    auto ep = core.peer_endpoint(999);
    EXPECT_FALSE(ep.has_value());
}

TEST(CoreTest, Subscribe_Unsubscribe) {
    Config config(true);
    config.plugins.auto_load = false;

    gn::Core core(&config);
    auto cb = [](std::string_view, std::shared_ptr<header_t>,
                 const endpoint_t*, gn::PacketData) -> propagation_t {
        return PROPAGATION_CONTINUE;
    };

    uint64_t sub_id = core.subscribe(MSG_TYPE_CHAT, "test_sub", cb);
    EXPECT_GT(sub_id, 0u);

    EXPECT_NO_THROW(core.unsubscribe(sub_id));
}

TEST(CoreTest, HandlerAndConnectorCount_NoPlugins) {
    Config config(true);
    config.plugins.auto_load = false;

    gn::Core core(&config);
    EXPECT_EQ(core.handler_count(), 0u);
    EXPECT_EQ(core.connector_count(), 0u);
}

TEST(CoreTest, DumpConnections_EmptyJson) {
    Config config(true);
    config.plugins.auto_load = false;

    gn::Core core(&config);
    auto json = core.dump_connections();
    EXPECT_EQ(json, "[]");
}

TEST(CoreTest, ReloadConfig_NoFile_ReturnsFalse) {
    Config config(true);
    config.plugins.auto_load = false;

    gn::Core core(&config);
    EXPECT_FALSE(core.reload_config());
}

TEST(CoreTest, ActiveUris_Empty) {
    Config config(true);
    config.plugins.auto_load = false;

    gn::Core core(&config);
    EXPECT_TRUE(core.active_uris().empty());
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 6: Additional C API coverage
// ═══════════════════════════════════════════════════════════════════════════════

TEST(CapiTest, IsRunning_Lifecycle) {
    auto dir = tmp_dir("capi_running");
    auto dir_str = dir.string();
    gn_config_t cfg{};
    cfg.config_dir = dir_str.c_str();
    cfg.log_level = "off";
    cfg.listen_port = 0;

    gn_core_t* core = gn_core_create(&cfg);
    ASSERT_NE(core, nullptr);

    EXPECT_EQ(gn_core_is_running(core), 0);

    gn_core_run_async(core, 1);
    EXPECT_EQ(gn_core_is_running(core), 1);

    gn_core_stop(core);
    EXPECT_EQ(gn_core_is_running(core), 0);

    gn_core_destroy(core);
    fs::remove_all(dir);
}

TEST(CapiTest, Send_NoCrash) {
    auto dir = tmp_dir("capi_send");
    auto dir_str = dir.string();
    gn_config_t cfg{};
    cfg.config_dir = dir_str.c_str();
    cfg.log_level = "off";
    cfg.listen_port = 0;

    gn_core_t* core = gn_core_create(&cfg);
    ASSERT_NE(core, nullptr);

    uint8_t data[] = {1, 2, 3};
    EXPECT_NO_THROW(gn_core_send(core, "tcp://10.0.0.1:9999", MSG_TYPE_CHAT,
                                  data, sizeof(data)));

    gn_core_destroy(core);
    fs::remove_all(dir);
}

TEST(CapiTest, Broadcast_NoCrash) {
    auto dir = tmp_dir("capi_bcast");
    auto dir_str = dir.string();
    gn_config_t cfg{};
    cfg.config_dir = dir_str.c_str();
    cfg.log_level = "off";
    cfg.listen_port = 0;

    gn_core_t* core = gn_core_create(&cfg);
    ASSERT_NE(core, nullptr);

    uint8_t data[] = {4, 5, 6};
    EXPECT_NO_THROW(gn_core_broadcast(core, MSG_TYPE_CHAT, data, sizeof(data)));

    gn_core_destroy(core);
    fs::remove_all(dir);
}

TEST(CapiTest, ConnectionCount_Zero) {
    auto dir = tmp_dir("capi_conncount");
    auto dir_str = dir.string();
    gn_config_t cfg{};
    cfg.config_dir = dir_str.c_str();
    cfg.log_level = "off";
    cfg.listen_port = 0;

    gn_core_t* core = gn_core_create(&cfg);
    ASSERT_NE(core, nullptr);

    EXPECT_EQ(gn_core_connection_count(core), 0u);

    gn_core_destroy(core);
    fs::remove_all(dir);
}

TEST(CapiTest, DumpConnections_Json) {
    auto dir = tmp_dir("capi_dump");
    auto dir_str = dir.string();
    gn_config_t cfg{};
    cfg.config_dir = dir_str.c_str();
    cfg.log_level = "off";
    cfg.listen_port = 0;

    gn_core_t* core = gn_core_create(&cfg);
    ASSERT_NE(core, nullptr);

    char buf[256]{};
    size_t len = gn_core_dump_connections(core, buf, sizeof(buf));
    EXPECT_GT(len, 0u);
    EXPECT_EQ(std::string(buf), "[]");

    gn_core_destroy(core);
    fs::remove_all(dir);
}

TEST(CapiTest, HandlerAndConnectorCount) {
    auto dir = tmp_dir("capi_hccount");
    auto dir_str = dir.string();
    gn_config_t cfg{};
    cfg.config_dir = dir_str.c_str();
    cfg.log_level = "off";
    cfg.listen_port = 0;

    gn_core_t* core = gn_core_create(&cfg);
    ASSERT_NE(core, nullptr);

    EXPECT_EQ(gn_core_handler_count(core), 0u);
    EXPECT_EQ(gn_core_connector_count(core), 0u);

    gn_core_destroy(core);
    fs::remove_all(dir);
}

TEST(CapiTest, Version_NotEmpty) {
    const char* ver = gn_version();
    ASSERT_NE(ver, nullptr);
    EXPECT_GT(strlen(ver), 0u);
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 7: Core thin wrappers coverage
// ═══════════════════════════════════════════════════════════════════════════════

TEST(CoreTest, SendByConnId_InvalidId) {
    Config config(true);
    config.plugins.auto_load = false;

    gn::Core core(&config);
    uint8_t data[] = {1, 2, 3};
    EXPECT_FALSE(core.send(conn_id_t(999), MSG_TYPE_CHAT,
                           std::span<const uint8_t>{data}));
}

TEST(CoreTest, Connect_NoCrash) {
    Config config(true);
    config.plugins.auto_load = false;

    gn::Core core(&config);
    EXPECT_NO_THROW(core.connect("tcp://10.0.0.1:9999"));
}

TEST(CoreTest, CloseNow_NoCrash) {
    Config config(true);
    config.plugins.auto_load = false;

    gn::Core core(&config);
    EXPECT_NO_THROW(core.close_now(999));
}

TEST(CoreTest, RotateIdentityKeys_NoCrash) {
    Config config(true);
    config.plugins.auto_load = false;

    gn::Core core(&config);
    EXPECT_NO_THROW(core.rotate_identity_keys());
}

TEST(CoreTest, RekeySession_InvalidConn) {
    Config config(true);
    config.plugins.auto_load = false;

    gn::Core core(&config);
    EXPECT_FALSE(core.rekey_session(999));
}

TEST(CoreTest, SubscribeWildcard_NoCrash) {
    Config config(true);
    config.plugins.auto_load = false;

    gn::Core core(&config);
    auto cb = [](std::string_view, std::shared_ptr<header_t>,
                 const endpoint_t*, gn::PacketData) -> propagation_t {
        return PROPAGATION_CONTINUE;
    };
    EXPECT_NO_THROW(core.subscribe_wildcard("test_wild", cb));
}

TEST(CoreTest, InternalAccessors) {
    Config config(true);
    config.plugins.auto_load = false;

    gn::Core core(&config);
    EXPECT_NO_THROW({
        [[maybe_unused]] auto& cm  = core.cm();
        [[maybe_unused]] auto& pm  = core.pm();
        [[maybe_unused]] auto& bus = core.bus();
    });
}

TEST(CoreTest, Disconnect_NoCrash) {
    Config config(true);
    config.plugins.auto_load = false;

    gn::Core core(&config);
    EXPECT_NO_THROW(core.disconnect(999));
}
