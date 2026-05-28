#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "lumen/detail/arena.h"
#include "lumen/detail/overflow_policy.h"
#include "lumen/detail/ring_buffer.h"
#include "lumen/record.h"

namespace lumen {
namespace {

// ── Stderr capture helpers (POSIX) ────────────────────────────────────────────

struct StderrCapture {
    int saved = -1;
    int read_fd = -1;
    int write_fd = -1;

    StderrCapture() {
        int p[2];
        pipe(p);
        saved = dup(STDERR_FILENO);
        dup2(p[1], STDERR_FILENO);
        read_fd = p[0];
        write_fd = p[1];
    }

    ~StderrCapture() {
        if (saved >= 0) {
            dup2(saved, STDERR_FILENO);
            close(saved);
        }
        if (read_fd >= 0) close(read_fd);
        if (write_fd >= 0) close(write_fd);
    }

    std::string str() {
        fflush(stderr);
        if (write_fd >= 0) {
            close(write_fd);
            write_fd = -1;
        }
        if (saved >= 0) {
            dup2(saved, STDERR_FILENO);
            close(saved);
            saved = -1;
        }

        std::string result;
        char buf[256];
        ssize_t n;
        while ((n = ::read(read_fd, buf, sizeof(buf) - 1)) > 0) {
            buf[n] = '\0';
            result += buf;
        }
        return result;
    }
};

// ── Arena tests ───────────────────────────────────────────────────────────────

TEST(ArenaTest, BasicAlloc) {
    Arena arena;

    char* p1 = arena.allocate(8);
    ASSERT_NE(p1, nullptr);
    std::memcpy(p1, "hello", 6);
    EXPECT_STREQ(p1, "hello");
    EXPECT_EQ(arena.remaining() + arena.used(), Arena::DEFAULT_CAPACITY);

    char* p2 = arena.allocate(16);
    ASSERT_NE(p2, nullptr);
    std::memcpy(p2, "world", 6);
    EXPECT_STREQ(p2, "world");

    // No overlap
    EXPECT_NE(p1, p2);
    EXPECT_STRNE(p1, "world");
    EXPECT_STREQ(p1, "hello");

    // Verify alignment (each pointer is max_align_t-aligned)
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p1) % alignof(std::max_align_t), 0);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p2) % alignof(std::max_align_t), 0);
}

TEST(ArenaTest, OverflowSpill) {
    StderrCapture cap;

    Arena arena;

    // Allocate enough to exhaust inline buffer
    std::vector<char*> ptrs;
    size_t total = 0;
    while (arena.remaining() > 0) {
        size_t chunk = std::min<size_t>(arena.remaining(), 512);
        char* p = arena.allocate(chunk);
        ASSERT_NE(p, nullptr);
        std::memset(p, 'A' + (ptrs.size() % 26), chunk - 1);
        p[chunk - 1] = '\0';
        ptrs.push_back(p);
        total += chunk;
    }

    // Now request more than inline capacity — triggers spill
    char* spilled = arena.allocate(256);
    ASSERT_NE(spilled, nullptr);
    std::memcpy(spilled, "spilled data", 13);
    EXPECT_STREQ(spilled, "spilled data");

    // Spilled pointer is different from any inline pointer
    for (char* p : ptrs) {
        EXPECT_NE(spilled, p);
    }

    // Verify warning was emitted on stderr
    std::string captured = cap.str();
    EXPECT_TRUE(captured.find("overflow") != std::string::npos);
}

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
