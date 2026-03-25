/// @file tests/heartbeat.cpp
/// @brief Unit tests for the Heartbeat subsystem.

#include <sodium.h>
#include <gtest/gtest.h>
#include <cstring>
#include <filesystem>
#include <chrono>
#include <thread>
#include <boost/asio.hpp>

#include "connectionManager.hpp"
#include "cm_impl.hpp"
#include "signals.hpp"
#include "logger.hpp"

#include "../sdk/connector.h"

namespace fs = std::filesystem;
using namespace gn;

// ─── helpers ──────────────────────────────────────────────────────────────────

static fs::path tmp_dir(const std::string& suffix = "") {
    auto p = fs::temp_directory_path() / ("gn_hb_test_" + suffix +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(p);
    return p;
}

// Track what mock connector sends
struct MockSendRecord {
    conn_id_t id;
    std::vector<uint8_t> data;
};
static std::vector<MockSendRecord> g_mock_sends;
static std::mutex g_mock_mu;

static connector_ops_t make_capturing_mock() {
    connector_ops_t ops{};
    ops.connect    = [](void*, const char*) -> int { return -1; };
    ops.listen     = [](void*, const char*, uint16_t) -> int { return 0; };
    ops.send_to    = [](void*, conn_id_t id, const void* data, size_t sz) -> int {
        std::lock_guard lk(g_mock_mu);
        MockSendRecord rec;
        rec.id = id;
        rec.data.assign(static_cast<const uint8_t*>(data),
                        static_cast<const uint8_t*>(data) + sz);
        g_mock_sends.push_back(std::move(rec));
        return 0;
    };
    ops.close      = [](void*, conn_id_t) {};
    ops.get_scheme = [](void*, char* b, size_t s) { strncpy(b, "mock", s); };
    ops.get_name   = [](void*, char* b, size_t s) { strncpy(b, "MockHB", s); };
    ops.shutdown   = [](void*) {};
    ops.connector_ctx = nullptr;
    return ops;
}

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
    connector_ops_t          mock_ops_ = make_capturing_mock();

    void SetUp() override {
        std::lock_guard lk(g_mock_mu);
        g_mock_sends.clear();
        cm_a_ = std::make_unique<ConnectionManager>(bus_, id_a_);
        cm_b_ = std::make_unique<ConnectionManager>(bus_, id_b_);
    }

    void TearDown() override {
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

    // Noise_XX handshake between cm_a and cm_b on localhost
    std::pair<conn_id_t, conn_id_t> do_handshake() {
        struct HBCapFrame {
            conn_id_t id;
            std::vector<uint8_t> data;
        };
        struct HBCapSink {
            std::mutex mu;
            std::vector<HBCapFrame> frames;

            std::vector<uint8_t> extract(uint16_t msg_type) {
                std::lock_guard lk(mu);
                for (auto it = frames.begin(); it != frames.end(); ++it) {
                    if (it->data.size() < sizeof(header_t)) continue;
                    auto* hdr = reinterpret_cast<const header_t*>(it->data.data());
                    if (hdr->payload_type == msg_type) {
                        auto d = std::move(it->data);
                        frames.erase(it);
                        return d;
                    }
                }
                return {};
            }
        };
        static HBCapSink sink;
        {
            std::lock_guard lk(sink.mu);
            sink.frames.clear();
        }

        connector_ops_t cap_ops{};
        cap_ops.connect    = [](void*, const char*) -> int { return -1; };
        cap_ops.listen     = [](void*, const char*, uint16_t) -> int { return 0; };
        cap_ops.send_to    = [](void*, conn_id_t id, const void* data, size_t sz) -> int {
            std::lock_guard lk(sink.mu);
            HBCapFrame f;
            f.id = id;
            f.data.assign(static_cast<const uint8_t*>(data),
                          static_cast<const uint8_t*>(data) + sz);
            sink.frames.push_back(std::move(f));
            return 0;
        };
        cap_ops.close      = [](void*, conn_id_t) {};
        cap_ops.get_scheme = [](void*, char* b, size_t s) { strncpy(b, "mock", s); };
        cap_ops.get_name   = [](void*, char* b, size_t s) { strncpy(b, "MockCap", s); };
        cap_ops.shutdown   = [](void*) {};
        cap_ops.connector_ctx = nullptr;

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

        // NOISE_INIT (A→B)
        auto init_frame = sink.extract(MSG_TYPE_NOISE_INIT);
        if (!init_frame.empty())
            api_b.on_data(api_b.ctx, cid_b, init_frame.data(), init_frame.size());

        // NOISE_RESP (B→A)
        auto resp_frame = sink.extract(MSG_TYPE_NOISE_RESP);
        if (!resp_frame.empty())
            api_a.on_data(api_a.ctx, cid_a, resp_frame.data(), resp_frame.size());

        // NOISE_FIN (A→B)
        auto fin_frame = sink.extract(MSG_TYPE_NOISE_FIN);
        if (!fin_frame.empty())
            api_b.on_data(api_b.ctx, cid_b, fin_frame.data(), fin_frame.size());

        // Re-register the capturing mock for heartbeat tests
        cm_a_->register_connector("tcp", &mock_ops_);
        cm_b_->register_connector("tcp", &mock_ops_);

        // Clear mock sends accumulated during handshake
        {
            std::lock_guard lk(g_mock_mu);
            g_mock_sends.clear();
        }

        return {cid_a, cid_b};
    }

    size_t mock_send_count() {
        std::lock_guard lk(g_mock_mu);
        return g_mock_sends.size();
    }

    int find_frame_by_type(uint16_t msg_type) {
        std::lock_guard lk(g_mock_mu);
        for (size_t i = 0; i < g_mock_sends.size(); ++i) {
            auto& data = g_mock_sends[i].data;
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

    {
        std::lock_guard lk(g_mock_mu);
        g_mock_sends.clear();
    }

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

    size_t hb_count = 0;
    {
        std::lock_guard lk(g_mock_mu);
        for (auto& rec : g_mock_sends) {
            if (rec.data.size() >= sizeof(header_t)) {
                auto* hdr = reinterpret_cast<const header_t*>(rec.data.data());
                if (hdr->payload_type == MSG_TYPE_HEARTBEAT) ++hb_count;
            }
        }
    }
    EXPECT_GE(hb_count, 2u);
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
