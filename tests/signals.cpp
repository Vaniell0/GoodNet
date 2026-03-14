/// @file tests/signals.cpp
/// SignalBus & PipelineSignal tests.

#include <gtest/gtest.h>
#include <boost/asio.hpp>
#include <atomic>
#include <thread>
#include <vector>

#include "signals.hpp"

using namespace gn;

// ─── Fixture ──────────────────────────────────────────────────────────────────

class SignalBusTest : public ::testing::Test {
protected:
    boost::asio::io_context ioc_;
    std::unique_ptr<SignalBus> bus_;

    void SetUp() override {
        bus_ = std::make_unique<SignalBus>(ioc_);
    }
};

// ═══════════════════════════════════════════════════════════════════════════════
// Packet pipeline
// ═══════════════════════════════════════════════════════════════════════════════

TEST_F(SignalBusTest, SubscribeDispatch_TypeSpecific) {
    std::atomic<int> called{0};
    bus_->subscribe(MSG_TYPE_CHAT, "h1",
        [&](std::string_view, std::shared_ptr<header_t>, const endpoint_t*, PacketData) {
            called.fetch_add(1);
            return PROPAGATION_CONTINUE;
        });

    auto hdr = std::make_shared<header_t>();
    hdr->payload_type = MSG_TYPE_CHAT;
    auto data = std::make_shared<sdk::RawBuffer>();
    bus_->dispatch_packet(MSG_TYPE_CHAT, hdr, nullptr, data);
    EXPECT_EQ(called.load(), 1);
}

TEST_F(SignalBusTest, SubscribeDispatch_Wildcard) {
    std::atomic<int> called{0};
    bus_->subscribe_wildcard("wild",
        [&](std::string_view, std::shared_ptr<header_t>, const endpoint_t*, PacketData) {
            called.fetch_add(1);
            return PROPAGATION_CONTINUE;
        });

    auto hdr = std::make_shared<header_t>();
    auto data = std::make_shared<sdk::RawBuffer>();

    bus_->dispatch_packet(MSG_TYPE_CHAT, hdr, nullptr, data);
    bus_->dispatch_packet(MSG_TYPE_HEARTBEAT, hdr, nullptr, data);
    EXPECT_EQ(called.load(), 2);
}

TEST_F(SignalBusTest, PriorityOrdering) {
    std::vector<int> order;
    auto make_cb = [&](int id) {
        return [&order, id](std::string_view, std::shared_ptr<header_t>,
                            const endpoint_t*, PacketData) -> propagation_t {
            order.push_back(id);
            return PROPAGATION_CONTINUE;
        };
    };

    bus_->subscribe(MSG_TYPE_CHAT, "lo",  make_cb(0),   0);
    bus_->subscribe(MSG_TYPE_CHAT, "mid", make_cb(128), 128);
    bus_->subscribe(MSG_TYPE_CHAT, "hi",  make_cb(255), 255);

    auto hdr = std::make_shared<header_t>();
    auto data = std::make_shared<sdk::RawBuffer>();
    bus_->dispatch_packet(MSG_TYPE_CHAT, hdr, nullptr, data);

    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], 255);
    EXPECT_EQ(order[1], 128);
    EXPECT_EQ(order[2], 0);
}

TEST_F(SignalBusTest, PropagationConsumed_StopsChain) {
    std::atomic<int> second_called{0};
    bus_->subscribe(MSG_TYPE_CHAT, "first",
        [](std::string_view, std::shared_ptr<header_t>, const endpoint_t*, PacketData) {
            return PROPAGATION_CONSUMED;
        }, 255);
    bus_->subscribe(MSG_TYPE_CHAT, "second",
        [&](std::string_view, std::shared_ptr<header_t>, const endpoint_t*, PacketData) {
            second_called.fetch_add(1);
            return PROPAGATION_CONTINUE;
        }, 0);

    auto hdr = std::make_shared<header_t>();
    auto data = std::make_shared<sdk::RawBuffer>();
    auto r = bus_->dispatch_packet(MSG_TYPE_CHAT, hdr, nullptr, data);

    EXPECT_EQ(r.result, PROPAGATION_CONSUMED);
    EXPECT_EQ(r.consumed_by, "first");
    EXPECT_EQ(second_called.load(), 0);
}

TEST_F(SignalBusTest, PropagationReject) {
    bus_->subscribe(MSG_TYPE_CHAT, "rejector",
        [](std::string_view, std::shared_ptr<header_t>, const endpoint_t*, PacketData) {
            return PROPAGATION_REJECT;
        });

    auto hdr = std::make_shared<header_t>();
    auto data = std::make_shared<sdk::RawBuffer>();
    auto r = bus_->dispatch_packet(MSG_TYPE_CHAT, hdr, nullptr, data);

    EXPECT_EQ(r.result, PROPAGATION_REJECT);
}

TEST_F(SignalBusTest, Unsubscribe_Removes) {
    std::atomic<int> called{0};
    uint64_t sub = bus_->subscribe(MSG_TYPE_CHAT, "removable",
        [&](std::string_view, std::shared_ptr<header_t>, const endpoint_t*, PacketData) {
            called.fetch_add(1);
            return PROPAGATION_CONTINUE;
        });

    auto hdr = std::make_shared<header_t>();
    auto data = std::make_shared<sdk::RawBuffer>();

    bus_->dispatch_packet(MSG_TYPE_CHAT, hdr, nullptr, data);
    EXPECT_EQ(called.load(), 1);

    bus_->unsubscribe(sub);
    bus_->dispatch_packet(MSG_TYPE_CHAT, hdr, nullptr, data);
    EXPECT_EQ(called.load(), 1);  // still 1 — unsubscribed
}

TEST_F(SignalBusTest, MultipleSubscribers_SameType) {
    std::atomic<int> count{0};
    auto cb = [&](std::string_view, std::shared_ptr<header_t>, const endpoint_t*, PacketData) {
        count.fetch_add(1);
        return PROPAGATION_CONTINUE;
    };

    bus_->subscribe(MSG_TYPE_CHAT, "a", cb);
    bus_->subscribe(MSG_TYPE_CHAT, "b", cb);

    auto hdr = std::make_shared<header_t>();
    auto data = std::make_shared<sdk::RawBuffer>();
    bus_->dispatch_packet(MSG_TYPE_CHAT, hdr, nullptr, data);

    EXPECT_EQ(count.load(), 2);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Stats accumulation
// ═══════════════════════════════════════════════════════════════════════════════

TEST_F(SignalBusTest, EmitStat_RxTxBytes) {
    bus_->emit_stat({StatsEvent::Kind::RxBytes, 100});
    bus_->emit_stat({StatsEvent::Kind::TxBytes, 200});
    bus_->emit_stat({StatsEvent::Kind::RxBytes, 50});

    auto snap = bus_->stats_snapshot();
    EXPECT_EQ(snap.rx_bytes, 150u);
    EXPECT_EQ(snap.tx_bytes, 200u);
}

TEST_F(SignalBusTest, EmitStat_Packets) {
    bus_->emit_stat({StatsEvent::Kind::RxPacket, 1});
    bus_->emit_stat({StatsEvent::Kind::RxPacket, 1});
    bus_->emit_stat({StatsEvent::Kind::TxPacket, 3});

    auto snap = bus_->stats_snapshot();
    EXPECT_EQ(snap.rx_packets, 2u);
    EXPECT_EQ(snap.tx_packets, 3u);
}

TEST_F(SignalBusTest, EmitStat_Auth) {
    bus_->emit_stat({StatsEvent::Kind::AuthOk, 1});
    bus_->emit_stat({StatsEvent::Kind::AuthOk, 1});
    bus_->emit_stat({StatsEvent::Kind::AuthFail, 1});

    auto snap = bus_->stats_snapshot();
    EXPECT_EQ(snap.auth_ok, 2u);
    EXPECT_EQ(snap.auth_fail, 1u);
}

TEST_F(SignalBusTest, EmitStat_ConnDisconn) {
    bus_->emit_stat({StatsEvent::Kind::Connect, 1});
    bus_->emit_stat({StatsEvent::Kind::Connect, 1});
    bus_->emit_stat({StatsEvent::Kind::Disconnect, 1});

    auto snap = bus_->stats_snapshot();
    EXPECT_EQ(snap.connections, 1u);
    EXPECT_EQ(snap.total_conn, 2u);
    EXPECT_EQ(snap.total_disc, 1u);
}

TEST_F(SignalBusTest, EmitDrop_AllReasons) {
    for (int i = 0; i < static_cast<int>(DropReason::_Count); ++i) {
        bus_->emit_drop(CONN_ID_INVALID, static_cast<DropReason>(i));
    }

    auto snap = bus_->stats_snapshot();
    for (int i = 0; i < static_cast<int>(DropReason::_Count); ++i) {
        EXPECT_EQ(snap.drops[i], 1u) << "drop reason index " << i;
    }
}

TEST_F(SignalBusTest, EmitLatency_Histogram) {
    // Record in the <1us bucket (index 0)
    bus_->emit_latency(CONN_ID_INVALID, 500);
    // Record in the <10us bucket (index 1)
    bus_->emit_latency(CONN_ID_INVALID, 5000);

    auto snap = bus_->stats_snapshot();
    EXPECT_EQ(snap.dispatch_latency.count.load(), 2u);
    EXPECT_EQ(snap.dispatch_latency.buckets[0].load(), 1u);
    EXPECT_EQ(snap.dispatch_latency.buckets[1].load(), 1u);
    EXPECT_EQ(snap.dispatch_latency.total_ns.load(), 5500u);
}

TEST_F(SignalBusTest, StatsSnapshot_InitiallyZero) {
    auto snap = bus_->stats_snapshot();
    EXPECT_EQ(snap.rx_bytes, 0u);
    EXPECT_EQ(snap.tx_bytes, 0u);
    EXPECT_EQ(snap.rx_packets, 0u);
    EXPECT_EQ(snap.tx_packets, 0u);
    EXPECT_EQ(snap.auth_ok, 0u);
    EXPECT_EQ(snap.auth_fail, 0u);
    EXPECT_EQ(snap.connections, 0u);
    EXPECT_EQ(snap.dispatch_latency.count.load(), 0u);
}

TEST_F(SignalBusTest, ConcurrentSubscribeDispatch) {
    std::atomic<int> total{0};
    auto cb = [&](std::string_view, std::shared_ptr<header_t>, const endpoint_t*, PacketData) {
        total.fetch_add(1);
        return PROPAGATION_CONTINUE;
    };

    bus_->subscribe(MSG_TYPE_CHAT, "base", cb);

    constexpr int N_THREADS = 4;
    constexpr int N_ITERS = 100;
    std::vector<std::thread> threads;

    for (int t = 0; t < N_THREADS; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < N_ITERS; ++i) {
                auto hdr = std::make_shared<header_t>();
                auto data = std::make_shared<sdk::RawBuffer>();
                bus_->dispatch_packet(MSG_TYPE_CHAT, hdr, nullptr, data);
            }
        });
    }

    for (auto& t : threads) t.join();
    EXPECT_GE(total.load(), N_THREADS * N_ITERS);
}
