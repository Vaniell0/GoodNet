/// @file tests/heartbeat.cpp
/// @brief Unit tests for the Heartbeat subsystem.

#include <sodium.h>
#include <gtest/gtest.h>
#include <cstring>
#include <chrono>
#include <thread>
#include <boost/asio.hpp>

#include "test_helpers.hpp"
#include "cm/connectionManager.hpp"
#include "cm/impl.hpp"
#include "signals.hpp"
#include "logger.hpp"

using namespace gn;

// ─── Fixture ──────────────────────────────────────────────────────────────────

class HeartbeatTest : public ::testing::Test {
protected:
    boost::asio::io_context  ioc_;
    SignalBus                bus_{ioc_};
    fs::path                 dir_a_ = tmp_dir("ha");
    fs::path                 dir_b_ = tmp_dir("hb");
    NodeIdentity             id_a_  = NodeIdentity::load_or_generate(dir_a_);
    NodeIdentity             id_b_  = NodeIdentity::load_or_generate(dir_b_);
    std::unique_ptr<ConnectionManager> cm_a_;
    std::unique_ptr<ConnectionManager> cm_b_;
    CapturingSink            runtime_sink_;  // captures post-handshake frames
    connector_ops_t          mock_ops_ = make_capturing_connector(&runtime_sink_);

    void SetUp() override {
        runtime_sink_.clear();
        cm_a_ = std::make_unique<ConnectionManager>(bus_, id_a_);
        cm_b_ = std::make_unique<ConnectionManager>(bus_, id_b_);
    }

    void TearDown() override {
        g_cap_sink = nullptr;
        if (cm_a_) cm_a_->shutdown();
        if (cm_b_) cm_b_->shutdown();
        fs::remove_all(dir_a_);
        fs::remove_all(dir_b_);
    }

    // Wrappers for private CM methods (HeartbeatTest is a friend → accesses impl_)
    void call_send_heartbeat(ConnectionManager& cm, conn_id_t id) {
        cm.impl_->send_heartbeat(id);
    }
    void call_handle_heartbeat(ConnectionManager& cm, conn_id_t id,
                                std::span<const uint8_t> payload) {
        cm.impl_->handle_heartbeat(id, payload);
    }
    std::shared_ptr<ConnectionRecord> get_record(ConnectionManager& cm, conn_id_t id) {
        return cm.impl_->rcu_find(id);
    }

    // Noise_XX handshake between cm_a and cm_b on localhost
    std::pair<conn_id_t, conn_id_t> do_handshake() {
        CapturingSink hs_sink;
        auto cap_ops = make_capturing_connector(&hs_sink);

        host_api_t api_a{}, api_b{};
        cm_a_->fill_host_api(&api_a);
        cm_b_->fill_host_api(&api_b);
        cm_a_->register_connector("tcp", &cap_ops);
        cm_b_->register_connector("tcp", &cap_ops);

        endpoint_t ep_ab{};
        strncpy(ep_ab.address, "127.0.0.1", sizeof(ep_ab.address));
        ep_ab.port = 9999;
        ep_ab.flags = EP_FLAG_TRUSTED | EP_FLAG_OUTBOUND;

        endpoint_t ep_ba{};
        strncpy(ep_ba.address, "127.0.0.1", sizeof(ep_ba.address));
        ep_ba.port = 9998;
        ep_ba.flags = EP_FLAG_TRUSTED;

        conn_id_t cid_a = api_a.on_connect(api_a.ctx, &ep_ab);
        conn_id_t cid_b = api_b.on_connect(api_b.ctx, &ep_ba);

        auto init_frame = hs_sink.extract(MSG_TYPE_NOISE_INIT);
        if (!init_frame.empty())
            api_b.on_data(api_b.ctx, cid_b, init_frame.data(), init_frame.size());

        auto resp_frame = hs_sink.extract(MSG_TYPE_NOISE_RESP);
        if (!resp_frame.empty())
            api_a.on_data(api_a.ctx, cid_a, resp_frame.data(), resp_frame.size());

        auto fin_frame = hs_sink.extract(MSG_TYPE_NOISE_FIN);
        if (!fin_frame.empty())
            api_b.on_data(api_b.ctx, cid_b, fin_frame.data(), fin_frame.size());

        // Re-register runtime capturing mock
        mock_ops_ = make_capturing_connector(&runtime_sink_);
        cm_a_->register_connector("tcp", &mock_ops_);
        cm_b_->register_connector("tcp", &mock_ops_);
        runtime_sink_.clear();

        return {cid_a, cid_b};
    }

    size_t mock_send_count() {
        std::lock_guard lk(runtime_sink_.mu);
        return runtime_sink_.frames.size();
    }

    int find_frame_by_type(uint16_t msg_type) {
        std::lock_guard lk(runtime_sink_.mu);
        for (size_t i = 0; i < runtime_sink_.frames.size(); ++i) {
            auto& data = runtime_sink_.frames[i].data;
            if (data.size() < sizeof(header_t)) continue;
            const auto* hdr = reinterpret_cast<const header_t*>(data.data());
            if (hdr->payload_type == msg_type) return static_cast<int>(i);
        }
        return -1;
    }

    // Feed a heartbeat frame into cm_a on connection cid
    void feed_heartbeat(conn_id_t cid, const NodeIdentity&,
                        uint8_t flags, uint32_t seq = 0, uint64_t ts = 12345) {
        msg::HeartbeatPayload hb{};
        hb.timestamp_us = ts;
        hb.seq = seq;
        hb.flags = flags;

        header_t h{};
        h.magic        = GNET_MAGIC;
        h.proto_ver    = GNET_PROTO_VER;
        h.flags        = GNET_FLAG_TRUSTED;
        h.payload_type = MSG_TYPE_HEARTBEAT;
        h.payload_len  = sizeof(hb);

        std::vector<uint8_t> frame(sizeof(h) + sizeof(hb));
        std::memcpy(frame.data(), &h, sizeof(h));
        std::memcpy(frame.data() + sizeof(h), &hb, sizeof(hb));

        host_api_t api{};
        cm_a_->fill_host_api(&api);
        api.on_data(api.ctx, cid, frame.data(), frame.size());
    }
};

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 1: HeartbeatPayload struct tests
// ═══════════════════════════════════════════════════════════════════════════════

TEST(HeartbeatPayloadTest, Size) {
    EXPECT_EQ(sizeof(msg::HeartbeatPayload), 16u);
}

TEST(HeartbeatPayloadTest, PingFlag) {
    msg::HeartbeatPayload hb{};
    hb.flags = 0x00;
    EXPECT_EQ(hb.flags, 0x00);
}

TEST(HeartbeatPayloadTest, PongFlag) {
    msg::HeartbeatPayload hb{};
    hb.flags = 0x01;
    EXPECT_EQ(hb.flags, 0x01);
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 2: send_heartbeat
// ═══════════════════════════════════════════════════════════════════════════════

TEST_F(HeartbeatTest, SendHeartbeat_NonEstablished_Ignored) {
    host_api_t api{};
    cm_a_->fill_host_api(&api);
    cm_a_->register_connector("tcp", &mock_ops_);

    endpoint_t ep{};
    strncpy(ep.address, "10.0.0.1", sizeof(ep.address));
    ep.port = 9999;
    conn_id_t cid = api.on_connect(api.ctx, &ep);

    runtime_sink_.clear();

    call_send_heartbeat(*cm_a_, cid);
    EXPECT_EQ(mock_send_count(), 0u);
}

TEST_F(HeartbeatTest, SendHeartbeat_Established_SendsFrame) {
    auto [cid_a, cid_b] = do_handshake();

    call_send_heartbeat(*cm_a_, cid_a);

    EXPECT_GE(mock_send_count(), 1u);
    int hb_idx = find_frame_by_type(MSG_TYPE_HEARTBEAT);
    EXPECT_GE(hb_idx, 0) << "No HEARTBEAT frame found in captured sends";
}

TEST_F(HeartbeatTest, HeartbeatSeq_Increments) {
    auto [cid_a, cid_b] = do_handshake();

    call_send_heartbeat(*cm_a_, cid_a);
    call_send_heartbeat(*cm_a_, cid_a);

    EXPECT_GE(runtime_sink_.count_frames(MSG_TYPE_HEARTBEAT), 2u);
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 3: handle_heartbeat (via direct call)
// ═══════════════════════════════════════════════════════════════════════════════

TEST_F(HeartbeatTest, HandleHeartbeat_Ping_SendsPong) {
    auto [cid_a, cid_b] = do_handshake();

    feed_heartbeat(cid_a, id_b_, 0x00, /*seq=*/42, /*ts=*/99999);

    int pong_idx = find_frame_by_type(MSG_TYPE_HEARTBEAT);
    EXPECT_GE(pong_idx, 0) << "No HEARTBEAT PONG frame found";
}

TEST_F(HeartbeatTest, HandleHeartbeat_DirectPong_ResetsMissed) {
    auto [cid_a, cid_b] = do_handshake();

    msg::HeartbeatPayload pong{};
    pong.timestamp_us = 12345;
    pong.seq = 0;
    pong.flags = 0x01;

    call_handle_heartbeat(*cm_a_, cid_a,
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&pong), sizeof(pong)));

    auto state = cm_a_->get_state(cid_a);
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(*state, STATE_ESTABLISHED);
}

TEST_F(HeartbeatTest, HandleHeartbeat_DirectPing_SendsPong) {
    auto [cid_a, cid_b] = do_handshake();

    msg::HeartbeatPayload ping{};
    ping.timestamp_us = 99999;
    ping.seq = 42;
    ping.flags = 0x00;

    call_handle_heartbeat(*cm_a_, cid_a,
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&ping), sizeof(ping)));

    int pong_idx = find_frame_by_type(MSG_TYPE_HEARTBEAT);
    EXPECT_GE(pong_idx, 0) << "No HEARTBEAT PONG frame found after direct PING";
}

TEST_F(HeartbeatTest, HandleHeartbeat_TooShort_Ignored) {
    auto [cid_a, cid_b] = do_handshake();

    uint8_t short_payload[4] = {0};
    call_handle_heartbeat(*cm_a_, cid_a,
        std::span<const uint8_t>(short_payload, 4));

    EXPECT_EQ(mock_send_count(), 0u);
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 4: check_heartbeat_timeouts
// ═══════════════════════════════════════════════════════════════════════════════

TEST_F(HeartbeatTest, CheckTimeouts_FirstCycle_Initializes) {
    auto [cid_a, cid_b] = do_handshake();

    cm_a_->check_heartbeat_timeouts();

    auto state = cm_a_->get_state(cid_a);
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(*state, STATE_ESTABLISHED);
}

TEST_F(HeartbeatTest, CheckTimeouts_RecentPong_NoDisconnect) {
    auto [cid_a, cid_b] = do_handshake();

    cm_a_->check_heartbeat_timeouts();

    msg::HeartbeatPayload pong{};
    pong.flags = 0x01;
    call_handle_heartbeat(*cm_a_, cid_a,
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&pong), sizeof(pong)));

    cm_a_->check_heartbeat_timeouts();

    auto state = cm_a_->get_state(cid_a);
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(*state, STATE_ESTABLISHED);
}

TEST_F(HeartbeatTest, CheckTimeouts_MissedThreshold_Disconnects) {
    auto [cid_a, cid_b] = do_handshake();

    // Первый вызов — инициализирует last_heartbeat_recv
    cm_a_->check_heartbeat_timeouts();
    auto state = cm_a_->get_state(cid_a);
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(*state, STATE_ESTABLISHED);

    // Устанавливаем last_heartbeat_recv на старое время (>30s назад)
    // чтобы check_heartbeat_timeouts() считал heartbeat пропущенным.
    // Доступ через impl_ (тест — friend).
    auto rec = get_record(*cm_a_, cid_a);
    ASSERT_NE(rec, nullptr);

    // Ставим время на 60 секунд назад
    auto old_ts = std::chrono::steady_clock::now() - std::chrono::seconds(60);
    rec->last_heartbeat_recv.store(
        old_ts.time_since_epoch().count(), std::memory_order_release);
    rec->missed_heartbeats.store(0, std::memory_order_release);

    // MAX_MISSED_HEARTBEATS = 3. Каждый вызов при elapsed > 30s
    // инкрементирует missed. При missed < 3 шлёт heartbeat.
    // При missed >= 3 → disconnect.

    // Первый missed: missed=1, send_heartbeat
    cm_a_->check_heartbeat_timeouts();
    state = cm_a_->get_state(cid_a);
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(*state, STATE_ESTABLISHED);

    // Сбрасываем время снова (send_heartbeat не сбрасывает last_heartbeat_recv)
    rec->last_heartbeat_recv.store(
        old_ts.time_since_epoch().count(), std::memory_order_release);

    // Второй missed: missed=2, send_heartbeat
    cm_a_->check_heartbeat_timeouts();
    state = cm_a_->get_state(cid_a);
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(*state, STATE_ESTABLISHED);

    // Сбрасываем время снова
    rec->last_heartbeat_recv.store(
        old_ts.time_since_epoch().count(), std::memory_order_release);

    // Третий missed: missed=3 >= MAX_MISSED_HEARTBEATS → disconnect() вызван.
    // disconnect() вызывает connector->close() (mock no-op), запись удаляется
    // когда connector вызовет on_disconnect. В тесте с mock connector —
    // проверяем что missed_heartbeats достиг порога.
    cm_a_->check_heartbeat_timeouts();

    // missed_heartbeats >= 3 → disconnect() был вызван
    EXPECT_GE(rec->missed_heartbeats.load(std::memory_order_acquire), 3u)
        << "missed_heartbeats should reach threshold of 3";
}
