#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "lumen/detail/overflow_policy.h"
#include "lumen/detail/ring_buffer.h"
#include "lumen/record.h"

namespace lumen {
namespace {

// ── Ring buffer tests ─────────────────────────────────────────────────────────

using SmallIntBuffer = RingBuffer<int, 4>;
using LargeIntBuffer = RingBuffer<int, 256>;

TEST(RingBufferTest, SingleProducerSingleConsumer) {
    LargeIntBuffer buf;

    constexpr int N = 200;
    for (int i = 0; i < N; ++i) {
        EXPECT_TRUE(buf.push(std::move(i)));
    }

    EXPECT_EQ(buf.size(), static_cast<uint64_t>(N));

    for (int i = 0; i < N; ++i) {
        int val = -1;
        EXPECT_TRUE(buf.pop(val));
        EXPECT_EQ(val, i);
    }

    EXPECT_TRUE(buf.empty());
}

TEST(RingBufferTest, MultiProducerSingleConsumer) {
    RingBuffer<int, 4096> buf;
    std::atomic<int> produced{0};

    constexpr int THREADS = 4;
    constexpr int PER_THREAD = 1000;
    constexpr int TOTAL = THREADS * PER_THREAD;

    std::vector<std::thread> producers;
    for (int t = 0; t < THREADS; ++t) {
        producers.emplace_back([&buf, &produced]() {
            for (int i = 0; i < PER_THREAD; ++i) {
                int val = produced.fetch_add(1, std::memory_order_relaxed);
                while (!buf.push(std::move(val))) {
                    std::this_thread::yield();
                }
            }
        });
    }

    // Drain while producers run
    std::vector<int> consumed;
    int expected_total = TOTAL;
    while (static_cast<int>(consumed.size()) < expected_total) {
        int val;
        if (buf.pop(val)) {
            consumed.push_back(val);
        } else {
            std::this_thread::yield();
        }
    }

    for (auto& t : producers) t.join();

    EXPECT_EQ(consumed.size(), static_cast<size_t>(TOTAL));

    // Every value 0..TOTAL-1 appears exactly once
    std::sort(consumed.begin(), consumed.end());
    for (int i = 0; i < TOTAL; ++i) {
        EXPECT_EQ(consumed[i], i);
    }
}

TEST(RingBufferTest, OverflowDropNewest) {
    SmallIntBuffer buf;

    // Fill the buffer
    for (int i = 0; i < 4; ++i) {
        EXPECT_TRUE((buf.push<OverflowPolicy::DROP_NEWEST>(i)));
    }
    EXPECT_EQ(buf.size(), 4);

    // Push a 5th item with DROP_NEWEST — should be dropped
    EXPECT_FALSE((buf.push<OverflowPolicy::DROP_NEWEST>(99)));

    // All original items preserved
    for (int i = 0; i < 4; ++i) {
        int val = -1;
        EXPECT_TRUE(buf.pop(val));
        EXPECT_EQ(val, i);
    }
    EXPECT_TRUE(buf.empty());
}

TEST(RingBufferTest, OverflowDropOldest) {
    SmallIntBuffer buf;

    // Fill the buffer
    for (int i = 0; i < 4; ++i) {
        EXPECT_TRUE((buf.push<OverflowPolicy::DROP_OLDEST>(i)));
    }
    EXPECT_EQ(buf.size(), 4);

    // Push a 5th item with DROP_OLDEST — drops item 0
    EXPECT_TRUE((buf.push<OverflowPolicy::DROP_OLDEST>(99)));

    // Item 0 was dropped; items 1, 2, 3, 99 survive
    // Consumer reads positions 1, 2, 3, 4
    int val = -1;
    EXPECT_TRUE(buf.pop(val));
    EXPECT_EQ(val, 1);   // item 0 dropped

    EXPECT_TRUE(buf.pop(val));
    EXPECT_EQ(val, 2);

    EXPECT_TRUE(buf.pop(val));
    EXPECT_EQ(val, 3);

    EXPECT_TRUE(buf.pop(val));
    EXPECT_EQ(val, 99);  // newest delivered

    EXPECT_TRUE(buf.empty());
}

TEST(RingBufferTest, OverflowBlock) {
    SmallIntBuffer buf;

    // Fill the buffer (items 0, 1, 2, 3)
    for (int i = 0; i < 4; ++i) {
        EXPECT_TRUE((buf.push<OverflowPolicy::BLOCK>(i)));
    }

    // Start a consumer thread that pops after a short delay
    std::thread consumer([&buf]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        int val = -1;
        EXPECT_TRUE(buf.pop(val));
        EXPECT_EQ(val, 0);
    });

    // Push a 5th item with BLOCK — producer blocks until consumer drains one
    EXPECT_TRUE((buf.push<OverflowPolicy::BLOCK>(99)));

    consumer.join();

    // Remaining items: 1, 2, 3, 99
    int val = -1;
    EXPECT_TRUE(buf.pop(val));
    EXPECT_EQ(val, 1);

    EXPECT_TRUE(buf.pop(val));
    EXPECT_EQ(val, 2);

    EXPECT_TRUE(buf.pop(val));
    EXPECT_EQ(val, 3);

    EXPECT_TRUE(buf.pop(val));
    EXPECT_EQ(val, 99);

    EXPECT_TRUE(buf.empty());
}

}  // namespace
}  // namespace lumen
