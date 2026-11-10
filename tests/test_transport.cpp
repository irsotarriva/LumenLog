#include <algorithm>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "lumen/detail/ring_buffer.hpp"

namespace lumen {
namespace {

struct LifetimeCounts {
    std::atomic<int64_t> constructed{0};
    std::atomic<int64_t> destroyed{0};
};

// Counts its constructions and destructions. The value lives on the heap, so a
// double destruction is a double free for ASan, and a hand-over between threads
// without synchronisation is a race on the heap value for TSan. Has no default
// constructor, so the buffer cannot create elements of its own.
class Tracked {
public:
    static constexpr uint64_t moved_from = std::numeric_limits<uint64_t>::max();

    Tracked(LifetimeCounts& counts, uint64_t value)
        : counts_(&counts), value_(std::make_unique<const uint64_t>(value)) {
        counts_->constructed.fetch_add(1, std::memory_order_relaxed);
    }
    Tracked(Tracked&& other) noexcept : counts_(other.counts_), value_(std::move(other.value_)) {
        counts_->constructed.fetch_add(1, std::memory_order_relaxed);
    }
    Tracked& operator=(Tracked&& other) noexcept {
        value_ = std::move(other.value_);
        return *this;
    }
    Tracked(const Tracked&) = delete;
    Tracked& operator=(const Tracked&) = delete;
    ~Tracked() { counts_->destroyed.fetch_add(1, std::memory_order_relaxed); }

    [[nodiscard]] uint64_t value() const { return value_ ? *value_ : moved_from; }

private:
    LifetimeCounts* counts_;  // non-owning
    std::unique_ptr<const uint64_t> value_;
};

struct ThrowingMove {
    ThrowingMove() = default;
    ThrowingMove(ThrowingMove&&) noexcept(false) {}
    ThrowingMove& operator=(ThrowingMove&&) noexcept(false) { return *this; }
};

template <typename T, std::size_t N>
concept CanInstantiate = requires { typename RingBuffer<T, N>; };

static_assert(CanInstantiate<int, 1>);
static_assert(CanInstantiate<Tracked, 1024>);
static_assert(!CanInstantiate<int, 0>, "capacity zero is rejected");
static_assert(!CanInstantiate<int, 3>, "a capacity that is not a power of two is rejected");
static_assert(!CanInstantiate<ThrowingMove, 4>, "an element whose move may throw is rejected");

// ── Single-threaded semantics ────────────────────────────────────────────────

TEST(RingBufferTest, TryPopReturnsPushedElementsInFifoOrder) {
    RingBuffer<int, 8> buffer;
    for (int i = 0; i < 8; ++i) {
        EXPECT_TRUE(buffer.try_push(int{i}));
    }
    for (int i = 0; i < 8; ++i) {
        int out = -1;
        EXPECT_TRUE(buffer.try_pop(out));
        EXPECT_EQ(out, i);
    }
}

TEST(RingBufferTest, TryPopOnEmptyBufferReturnsFalseAndLeavesOutputUntouched) {
    RingBuffer<int, 4> buffer;
    int out = 42;
    EXPECT_FALSE(buffer.try_pop(out));
    EXPECT_EQ(out, 42);

    EXPECT_TRUE(buffer.try_push(1));
    EXPECT_TRUE(buffer.try_pop(out));
    out = 42;
    EXPECT_FALSE(buffer.try_pop(out));
    EXPECT_EQ(out, 42);
}

TEST(RingBufferTest, TryPushOnFullBufferReturnsFalseAndLeavesItemIntact) {
    RingBuffer<std::unique_ptr<int>, 2> buffer;
    EXPECT_TRUE(buffer.try_push(std::make_unique<int>(0)));
    EXPECT_TRUE(buffer.try_push(std::make_unique<int>(1)));

    auto item = std::make_unique<int>(99);
    EXPECT_FALSE(buffer.try_push(std::move(item)));
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(*item, 99);

    std::unique_ptr<int> out;
    ASSERT_TRUE(buffer.try_pop(out));
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(*out, 0);
    EXPECT_TRUE(buffer.try_push(std::move(item)));
    EXPECT_EQ(item, nullptr);
}

TEST(RingBufferTest, RejectedCountCountsOnlyFailedTryPushes) {
    RingBuffer<int, 2> buffer;
    EXPECT_EQ(buffer.rejected_count(), 0u);
    EXPECT_TRUE(buffer.try_push(1));
    EXPECT_TRUE(buffer.try_push(2));
    EXPECT_EQ(buffer.rejected_count(), 0u);

    EXPECT_FALSE(buffer.try_push(3));
    EXPECT_FALSE(buffer.try_push(4));
    EXPECT_FALSE(buffer.try_push(5));
    EXPECT_EQ(buffer.rejected_count(), 3u);

    int out = 0;
    EXPECT_TRUE(buffer.try_pop(out));
    EXPECT_TRUE(buffer.try_push(6));
    EXPECT_TRUE(buffer.try_pop(out));
    buffer.push_blocking(7);
    EXPECT_EQ(buffer.rejected_count(), 3u);
}

TEST(RingBufferTest, SizeEmptyAndPositionsFollowPushesAndPops) {
    RingBuffer<int, 4> buffer;
    EXPECT_TRUE(buffer.empty());
    EXPECT_EQ(buffer.size(), 0u);
    EXPECT_EQ(buffer.capacity(), 4u);

    EXPECT_TRUE(buffer.try_push(1));
    EXPECT_TRUE(buffer.try_push(2));
    EXPECT_TRUE(buffer.try_push(3));
    EXPECT_FALSE(buffer.empty());
    EXPECT_EQ(buffer.size(), 3u);
    EXPECT_EQ(buffer.head_position(), 3u);
    EXPECT_EQ(buffer.tail_position(), 0u);

    int out = 0;
    EXPECT_TRUE(buffer.try_pop(out));
    EXPECT_EQ(buffer.size(), 2u);
    EXPECT_EQ(buffer.tail_position(), 1u);

    EXPECT_TRUE(buffer.try_push(4));
    EXPECT_TRUE(buffer.try_push(5));
    EXPECT_FALSE(buffer.try_push(6));
    EXPECT_EQ(buffer.size(), 4u);
    EXPECT_EQ(buffer.head_position(), 5u);
}

TEST(RingBufferTest, PushBlockingIntoBufferWithRoomStoresElementWithoutRejecting) {
    RingBuffer<int, 2> buffer;
    buffer.push_blocking(10);
    buffer.push_blocking(11);
    int out = 0;
    EXPECT_TRUE(buffer.try_pop(out));
    EXPECT_EQ(out, 10);
    EXPECT_TRUE(buffer.try_pop(out));
    EXPECT_EQ(out, 11);
    EXPECT_EQ(buffer.rejected_count(), 0u);
}

TEST(RingBufferTest, WrapsAroundManyTimesPastCapacityInFifoOrder) {
    RingBuffer<uint64_t, 4> buffer;
    constexpr uint64_t laps = 1000;
    uint64_t next_in = 0;
    uint64_t next_out = 0;
    for (uint64_t lap = 0; lap < laps; ++lap) {
        // Vary the fill level so that every slot is seen both as head and tail.
        const uint64_t batch = 1 + lap % 4;
        for (uint64_t i = 0; i < batch; ++i) {
            EXPECT_TRUE(buffer.try_push(uint64_t{next_in++}));
        }
        for (uint64_t i = 0; i < batch; ++i) {
            uint64_t out = 0;
            EXPECT_TRUE(buffer.try_pop(out));
            EXPECT_EQ(out, next_out++);
        }
    }
    EXPECT_TRUE(buffer.empty());
    EXPECT_EQ(buffer.head_position(), next_in);
    EXPECT_EQ(buffer.tail_position(), next_in);
    EXPECT_GT(next_in, 100 * buffer.capacity());
    EXPECT_EQ(buffer.rejected_count(), 0u);
}

TEST(RingBufferTest, CapacityOneHoldsExactlyOneElementAtATime) {
    RingBuffer<int, 1> buffer;
    for (int i = 0; i < 100; ++i) {
        EXPECT_TRUE(buffer.try_push(int{i}));
        EXPECT_FALSE(buffer.try_push(-1));
        int out = -1;
        EXPECT_TRUE(buffer.try_pop(out));
        EXPECT_EQ(out, i);
        EXPECT_FALSE(buffer.try_pop(out));
    }
    EXPECT_EQ(buffer.rejected_count(), 100u);
}

// ── Element lifetime ─────────────────────────────────────────────────────────

TEST(RingBufferTest, ConstructingAndDestroyingEmptyBufferCreatesNoElements) {
    LifetimeCounts counts;
    { const RingBuffer<Tracked, 16> buffer; }
    EXPECT_EQ(counts.constructed.load(), 0);
    EXPECT_EQ(counts.destroyed.load(), 0);
}

TEST(RingBufferTest, DestroyingBufferReleasesUnconsumedSharedPtrsOnce) {
    const auto payload = std::make_shared<int>(7);
    {
        RingBuffer<std::shared_ptr<int>, 4> buffer;
        EXPECT_TRUE(buffer.try_push(std::shared_ptr<int>(payload)));
        EXPECT_TRUE(buffer.try_push(std::shared_ptr<int>(payload)));
        EXPECT_TRUE(buffer.try_push(std::shared_ptr<int>(payload)));
        std::shared_ptr<int> out;
        EXPECT_TRUE(buffer.try_pop(out));
        EXPECT_EQ(payload.use_count(), 4);
    }
    EXPECT_EQ(payload.use_count(), 1);
}

TEST(RingBufferTest, EveryElementIsDestroyedExactlyOnceIncludingUnconsumedAfterWrap) {
    LifetimeCounts counts;
    {
        RingBuffer<Tracked, 4> buffer;
        Tracked out(counts, 0);
        // Six pushes and pops move the tail to slot 2, so the three elements
        // left behind occupy slots 2, 3 and 0.
        for (uint64_t i = 0; i < 6; ++i) {
            EXPECT_TRUE(buffer.try_push(Tracked(counts, i)));
            EXPECT_TRUE(buffer.try_pop(out));
            EXPECT_EQ(out.value(), i);
        }
        for (uint64_t i = 6; i < 9; ++i) {
            EXPECT_TRUE(buffer.try_push(Tracked(counts, i)));
        }
        EXPECT_EQ(buffer.size(), 3u);
    }
    EXPECT_GT(counts.constructed.load(), 0);
    EXPECT_EQ(counts.destroyed.load(), counts.constructed.load());
}

// ── Multi-producer stress ────────────────────────────────────────────────────

constexpr uint64_t stress_producers = 4;
constexpr uint64_t stress_items_per_producer = 20'000;
constexpr uint64_t stress_total = stress_producers * stress_items_per_producer;

struct StressOutcome {
    std::vector<uint32_t> times_delivered = std::vector<uint32_t>(stress_total, 0);
    uint64_t delivered = 0;
    uint64_t rejected_seen_by_producers = 0;
    uint64_t invalid_values = 0;
    bool per_producer_order_kept = true;

    [[nodiscard]] int64_t values_delivered_more_than_once() const {
        return std::ranges::count_if(times_delivered, [](uint32_t times) { return times > 1; });
    }
    [[nodiscard]] int64_t values_not_delivered_once() const {
        return std::ranges::count_if(times_delivered, [](uint32_t times) { return times != 1; });
    }
};

// Value `index` of producer `producer`; each producer offers its values in
// increasing order, so the consumer can check FIFO order per producer.
[[nodiscard]] constexpr uint64_t stress_value(uint64_t producer, uint64_t index) {
    return producer * stress_items_per_producer + index;
}

void record_delivery(const Tracked& item, std::vector<uint64_t>& next_index, StressOutcome& outcome) {
    const uint64_t value = item.value();
    if (value >= stress_total) {
        ++outcome.invalid_values;
        return;
    }
    ++outcome.delivered;
    ++outcome.times_delivered[value];
    const uint64_t producer = value / stress_items_per_producer;
    const uint64_t index = value % stress_items_per_producer;
    if (index < next_index[producer]) {
        outcome.per_producer_order_kept = false;
    }
    next_index[producer] = index + 1;
}

// Starts the producers, each offering its values through `push` (which returns
// whether the value was accepted), and pops on this thread until every
// producer has returned and the buffer is empty.
template <std::size_t N, std::predicate<RingBuffer<Tracked, N>&, Tracked&&> Push>
[[nodiscard]] StressOutcome run_stress(RingBuffer<Tracked, N>& buffer, LifetimeCounts& counts,
                                       Push push) {
    StressOutcome outcome;
    std::atomic<uint64_t> finished{0};
    std::atomic<uint64_t> rejected{0};
    std::vector<std::thread> producers;
    for (uint64_t p = 0; p < stress_producers; ++p) {
        producers.emplace_back([&, p] {
            uint64_t my_rejected = 0;
            for (uint64_t i = 0; i < stress_items_per_producer; ++i) {
                if (!push(buffer, Tracked(counts, stress_value(p, i)))) {
                    ++my_rejected;
                }
            }
            rejected.fetch_add(my_rejected, std::memory_order_relaxed);
            finished.fetch_add(1, std::memory_order_release);
        });
    }

    std::vector<uint64_t> next_index(stress_producers, 0);
    Tracked out(counts, Tracked::moved_from);
    while (true) {
        // Read before popping: once every producer has returned, every
        // accepted element is published, so a failed pop means empty.
        const bool producers_done = finished.load(std::memory_order_acquire) == stress_producers;
        if (buffer.try_pop(out)) {
            record_delivery(out, next_index, outcome);
        } else if (producers_done) {
            break;
        } else {
            std::this_thread::yield();
        }
    }
    for (std::thread& t : producers) {
        t.join();
    }
    outcome.rejected_seen_by_producers = rejected.load();
    return outcome;
}

TEST(RingBufferStressTest, TryPushFromFourProducersDeliversOrRejectsEachValueOnce) {
    LifetimeCounts counts;
    {
        RingBuffer<Tracked, 64> buffer;
        const StressOutcome outcome = run_stress(
            buffer, counts, [](auto& b, Tracked&& item) { return b.try_push(std::move(item)); });

        EXPECT_EQ(outcome.invalid_values, 0u);
        EXPECT_EQ(outcome.values_delivered_more_than_once(), 0);
        EXPECT_EQ(outcome.delivered + outcome.rejected_seen_by_producers, stress_total);
        EXPECT_EQ(buffer.rejected_count(), outcome.rejected_seen_by_producers);
        EXPECT_TRUE(outcome.per_producer_order_kept);
        EXPECT_TRUE(buffer.empty());
        RecordProperty("delivered", std::to_string(outcome.delivered));
        RecordProperty("rejected", std::to_string(outcome.rejected_seen_by_producers));
    }
    EXPECT_EQ(counts.destroyed.load(), counts.constructed.load());
}

TEST(RingBufferStressTest, TryPushRetriedUntilAcceptedDeliversEveryValueExactlyOnce) {
    LifetimeCounts counts;
    {
        RingBuffer<Tracked, 2> buffer;
        const StressOutcome outcome = run_stress(buffer, counts, [](auto& b, Tracked&& item) {
            // A rejected push must leave `item` intact for the next attempt.
            while (!b.try_push(std::move(item))) {
                std::this_thread::yield();
            }
            return true;
        });

        EXPECT_EQ(outcome.invalid_values, 0u);
        EXPECT_EQ(outcome.values_not_delivered_once(), 0);
        EXPECT_EQ(outcome.delivered, stress_total);
        EXPECT_TRUE(outcome.per_producer_order_kept);
    }
    EXPECT_EQ(counts.destroyed.load(), counts.constructed.load());
}

TEST(RingBufferStressTest, PushBlockingFromFourProducersDeliversEveryValueExactlyOnce) {
    LifetimeCounts counts;
    {
        RingBuffer<Tracked, 2> buffer;
        const StressOutcome outcome = run_stress(buffer, counts, [](auto& b, Tracked&& item) {
            b.push_blocking(std::move(item));
            return true;
        });

        EXPECT_EQ(outcome.invalid_values, 0u);
        EXPECT_EQ(outcome.values_not_delivered_once(), 0);
        EXPECT_EQ(outcome.delivered, stress_total);
        EXPECT_EQ(buffer.rejected_count(), 0u);
        EXPECT_TRUE(outcome.per_producer_order_kept);
    }
    EXPECT_EQ(counts.destroyed.load(), counts.constructed.load());
}

}  // namespace
}  // namespace lumen
