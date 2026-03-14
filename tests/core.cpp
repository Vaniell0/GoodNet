#include <gtest/gtest.h>
#include <core.hpp>
#include "core.h"

#include <cstring>

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 1: Core C++ lifecycle
// ═══════════════════════════════════════════════════════════════════════════════

TEST(CoreTest, FastStartup) {
    gn::CoreConfig cfg;
    cfg.network.io_threads = 1;
    cfg.plugins.auto_load = false;

    gn::Core core(cfg);
    core.run_async();
    EXPECT_TRUE(core.is_running());
    core.stop();
}

TEST(CoreTest, SingletonGuard) {
    gn::CoreConfig cfg;
    cfg.network.io_threads = 1;
    cfg.plugins.auto_load = false;

    gn::Core first(cfg);
    EXPECT_THROW({
        gn::Core second(cfg);
    }, std::runtime_error);
}

TEST(CoreTest, SequentialLifecycle) {
    gn::CoreConfig cfg;
    cfg.network.io_threads = 1;
    cfg.plugins.auto_load = false;

    // First instance: create, run, stop, destroy.
    {
        gn::Core core(cfg);
        core.run_async();
        core.stop();
    }

    // Second instance: singleton released by destructor — must not throw.
    {
        gn::Core core(cfg);
        core.run_async();
        core.stop();
    }
}

TEST(CoreTest, MultipleRunAsyncStopCycles) {
    gn::CoreConfig cfg;
    cfg.network.io_threads = 1;
    cfg.plugins.auto_load = false;

    gn::Core core(cfg);
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
    gn_config_t cfg{};
    cfg.config_dir = "./test_id";
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
}

TEST(CapiTest, StatsInitiallyZero) {
    gn_config_t cfg{};
    cfg.config_dir = "./test_id_stats";
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
}

TEST(CapiTest, SubscribeUnsubscribe) {
    gn_config_t cfg{};
    cfg.config_dir = "./test_id_sub";
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
    gn::CoreConfig cfg;
    cfg.network.io_threads = 1;
    cfg.plugins.auto_load = false;

    gn::Core core(cfg);
    core.run_async();

    // send to unknown URI — should not crash, just return false
    bool ok = core.send("tcp://10.0.0.99:9999", MSG_TYPE_CHAT, "hello", 5);
    EXPECT_FALSE(ok);

    core.stop();
}

TEST(CoreTest, CoreBroadcast_DoesNotCrash) {
    gn::CoreConfig cfg;
    cfg.network.io_threads = 1;
    cfg.plugins.auto_load = false;

    gn::Core core(cfg);
    core.run_async();

    uint8_t data[] = {1, 2, 3};
    EXPECT_NO_THROW(core.broadcast(MSG_TYPE_CHAT, data, sizeof(data)));

    core.stop();
}

TEST(CoreTest, ActiveConnIds_EmptyInitially) {
    gn::CoreConfig cfg;
    cfg.network.io_threads = 1;
    cfg.plugins.auto_load = false;

    gn::Core core(cfg);
    auto ids = core.active_conn_ids();
    EXPECT_TRUE(ids.empty());
}

TEST(CoreTest, StatsSnapshot_Valid) {
    gn::CoreConfig cfg;
    cfg.network.io_threads = 1;
    cfg.plugins.auto_load = false;

    gn::Core core(cfg);
    auto snap = core.stats_snapshot();
    EXPECT_EQ(snap.rx_bytes, 0u);
    EXPECT_EQ(snap.connections, 0u);
}

TEST(CoreTest, ConnectionCount_Zero) {
    gn::CoreConfig cfg;
    cfg.network.io_threads = 1;
    cfg.plugins.auto_load = false;

    gn::Core core(cfg);
    EXPECT_EQ(core.connection_count(), 0u);
}
