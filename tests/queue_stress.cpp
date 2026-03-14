/// @file tests/queue_stress.cpp
/// @brief Concurrent stress tests for PerConnQueue.

#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <vector>
#include <numeric>

#include "connectionManager.hpp"

using namespace gn;

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 1: PerConnQueue — concurrent push/drain
// ═══════════════════════════════════════════════════════════════════════════════

TEST(PerConnQueueTest, BasicPushAndDrain) {
    PerConnQueue q;
    std::vector<uint8_t> frame(100, 0xAA);

    EXPECT_TRUE(q.try_push(frame));
    EXPECT_EQ(q.pending_bytes.load(), 100u);

    auto batch = q.drain_batch(64);
    EXPECT_EQ(batch.size(), 1u);
    EXPECT_EQ(q.pending_bytes.load(), 0u);
}

TEST(PerConnQueueTest, BackpressureLimitRespected) {
    PerConnQueue q;
    const size_t frame_size = 1024 * 1024;  // 1 MB
    std::vector<uint8_t> frame(frame_size, 0xBB);

    int accepted = 0;
    // MAX_BYTES = 8 MB, so at most 8 frames of 1 MB should be accepted.
    for (int i = 0; i < 20; ++i) {
        if (q.try_push(frame))
            ++accepted;
    }

    EXPECT_EQ(accepted, 8);
    EXPECT_LE(q.pending_bytes.load(), PerConnQueue::MAX_BYTES);
}

TEST(PerConnQueueTest, DrainBatchRespectsLimit) {
    PerConnQueue q;
    for (int i = 0; i < 10; ++i) {
        q.try_push(std::vector<uint8_t>(100, static_cast<uint8_t>(i)));
    }

    auto batch = q.drain_batch(3);
    EXPECT_EQ(batch.size(), 3u);
    EXPECT_EQ(q.pending_bytes.load(), 700u);

    auto rest = q.drain_batch(64);
    EXPECT_EQ(rest.size(), 7u);
    EXPECT_EQ(q.pending_bytes.load(), 0u);
}

TEST(PerConnQueueTest, ConcurrentPushDrain) {
    PerConnQueue q;
    constexpr int NUM_PUSHERS = 4;
    constexpr int FRAMES_PER_PUSHER = 500;
    constexpr size_t FRAME_SIZE = 128;

    std::atomic<int> total_pushed{0};
    std::atomic<int> total_drained{0};
    std::atomic<bool> done{false};

    // Pusher threads.
    std::vector<std::thread> pushers;
    pushers.reserve(NUM_PUSHERS);
    for (int t = 0; t < NUM_PUSHERS; ++t) {
        pushers.emplace_back([&, t] {
            std::vector<uint8_t> frame(FRAME_SIZE, static_cast<uint8_t>(t));
            for (int i = 0; i < FRAMES_PER_PUSHER; ++i) {
                // Retry on backpressure — drain thread will free space.
                while (!q.try_push(frame)) {
                    std::this_thread::yield();
                }
                total_pushed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // Drainer thread.
    std::thread drainer([&] {
        while (!done.load(std::memory_order_relaxed) ||
               q.pending_bytes.load(std::memory_order_relaxed) > 0) {
            auto batch = q.drain_batch(32);
            total_drained.fetch_add(static_cast<int>(batch.size()),
                                     std::memory_order_relaxed);
            if (batch.empty())
                std::this_thread::yield();
        }
    });

    for (auto& t : pushers) t.join();
    done.store(true, std::memory_order_relaxed);
    drainer.join();

    // All pushed frames must be drained — no data loss.
    const int expected = NUM_PUSHERS * FRAMES_PER_PUSHER;
    EXPECT_EQ(total_pushed.load(), expected);
    EXPECT_EQ(total_drained.load(), expected);
    EXPECT_EQ(q.pending_bytes.load(), 0u);
}

TEST(PerConnQueueTest, ConcurrentPushBackpressureNoOverflow) {
    PerConnQueue q;
    constexpr int NUM_THREADS = 8;
    constexpr size_t FRAME_SIZE = 512 * 1024;  // 512 KB

    std::atomic<int> accepted{0};
    std::atomic<int> rejected{0};

    // All threads try to push simultaneously without draining.
    std::vector<std::thread> threads;
    threads.reserve(NUM_THREADS);
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&] {
            std::vector<uint8_t> frame(FRAME_SIZE, 0xCC);
            for (int i = 0; i < 50; ++i) {
                if (q.try_push(frame))
                    accepted.fetch_add(1, std::memory_order_relaxed);
                else
                    rejected.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& t : threads) t.join();

    // With fetch_add + rollback, pending_bytes should never exceed MAX_BYTES.
    EXPECT_LE(q.pending_bytes.load(), PerConnQueue::MAX_BYTES);
    // At most 8 MB / 512 KB = 16 frames can fit.
    EXPECT_LE(accepted.load(), 16);
    EXPECT_GT(rejected.load(), 0);
}

TEST(PerConnQueueTest, EmptyDrainReturnsEmpty) {
    PerConnQueue q;
    auto batch = q.drain_batch(64);
    EXPECT_TRUE(batch.empty());
    EXPECT_EQ(q.pending_bytes.load(), 0u);
}

TEST(PerConnQueueTest, DrainingFlagCanBeSet) {
    PerConnQueue q;
    EXPECT_FALSE(q.draining.load());
    q.draining.store(true);
    EXPECT_TRUE(q.draining.load());
}
