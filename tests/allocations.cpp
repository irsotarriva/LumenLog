// Heap allocations a producer statement makes on its own thread, in the
// steady state (the thread has logged before and its copy of the process
// tags is current). The global operator new is replaced in this executable
// and counts, per thread, the allocations of the thread that calls it; the
// dispatch thread's work is not counted. The numbers asserted here are the
// ones lumen/core.hpp ("Cost of a statement") states.

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <gtest/gtest.h>

#include "allocation_counter.hpp"
#include "lumen/lumen.hpp"
#include "test_support.hpp"

namespace lumen {
namespace {

using test::allocations_during;

class ProducerAllocations : public test::GlobalCoreTest {
protected:
    void SetUp() override {
        test::GlobalCoreTest::SetUp();
        // The first statement on a thread creates its context.
        LUMEN_LOG_INFO("warm up");
        core().flush();
    }
};

TEST_F(ProducerAllocations, ReplacedOperatorNewCountsTheCallingThreadsAllocations) {
    // Kept outside the lambda, so that the compiler cannot elide the allocation.
    std::unique_ptr<int64_t> held;
    EXPECT_EQ(allocations_during([&held] { held = std::make_unique<int64_t>(1); }), 1U);
    EXPECT_EQ(allocations_during([] {}), 0U);
    uint64_t on_other_thread = 0;
    std::unique_ptr<int64_t> held_there;
    std::thread other([&on_other_thread, &held_there] {
        on_other_thread =
            allocations_during([&held_there] { held_there = std::make_unique<int64_t>(2); });
    });
    const uint64_t here = allocations_during([&other] { other.join(); });
    EXPECT_EQ(on_other_thread, 1U);
    EXPECT_EQ(here, 0U);
    ASSERT_NE(held, nullptr);
    EXPECT_EQ(*held, 1);
}

TEST_F(ProducerAllocations, PlainLogStatementAllocatesOnce) {
    EXPECT_EQ(allocations_during([] { LUMEN_LOG_INFO("plain message"); }), 1U);
    core().flush();
    EXPECT_EQ(_captured->messages().back(), "plain message");
}

TEST_F(ProducerAllocations, FormattedLogStatementWithTwoTagsAllocatesOnce) {
    EXPECT_EQ(
        allocations_during([] { LUMEN_LOG_INFO("value {}", 42).tag("run", "r1").tag("event", 7); }),
        1U);
}

TEST_F(ProducerAllocations, MetricAllocatesOnce) {
    EXPECT_EQ(allocations_during([] { lumen::metric("loss", 0.25).tag("epoch", 3); }), 1U);
}

TEST_F(ProducerAllocations, ProgressUpdateAllocatesOnce) {
    ProgressHandle bar = progress("items", 10);
    EXPECT_EQ(allocations_during([&bar] { bar.tick(); }), 1U);
}

TEST_F(ProducerAllocations, AmbientTagsOfEveryLayerAddNoAllocation) {
    ASSERT_TRUE(set_process_tag("host", "node-1").has_value());
    ASSERT_TRUE(set_thread_tag("worker", "3").has_value());
    LUMEN_SCOPE("run", "12");
    LUMEN_LOG_INFO("refresh the thread's copy of the process tags");
    EXPECT_EQ(allocations_during([] { LUMEN_LOG_INFO("with context"); }), 1U);
    core().flush();
    const auto logs = _captured->logs();
    ASSERT_FALSE(logs.empty());
    EXPECT_EQ(logs.back().tags.count(), 3U);
}

TEST_F(ProducerAllocations, FirstLogAfterAProcessTagChangeAllocatesOnce) {
    ASSERT_TRUE(set_process_tag("host", "node-1").has_value());
    EXPECT_EQ(allocations_during([] { LUMEN_LOG_INFO("first after the change"); }), 1U);
}

TEST_F(ProducerAllocations, MessageLongerThanTheBuilderBufferAddsTwoAllocations) {
    const std::string long_text(detail::StringScratch::inline_capacity + 88, 'x');
    EXPECT_EQ(allocations_during([&long_text] { LUMEN_LOG_INFO(long_text); }), 3U);
}

TEST_F(ProducerAllocations, FormattedMessageLongerThanTheBuilderBufferAddsTwoAllocations) {
    const std::string long_text(detail::StringScratch::inline_capacity + 88, 'x');
    EXPECT_EQ(allocations_during([&long_text] { LUMEN_LOG_INFO("text: {}", long_text); }), 3U);
    core().flush();
    EXPECT_EQ(_captured->messages().back(), "text: " + long_text);
}

// The error path: a record refused by a full queue was prepared before the
// queue refused it, so it costs its allocation too, and nothing more.
TEST(ProducerAllocationsWhenRefused, RecordRefusedByAFullQueueAllocatesOnce) {
    struct Gate {
        std::mutex mutex;
        std::condition_variable changed;  // waits on mutex
        bool entered = false;             // guarded by mutex
        bool open = false;                // guarded by mutex
    };
    // Holds the dispatch thread in its first on_progress call until opened.
    class BlockingSink final : public Sink {
    public:
        explicit BlockingSink(std::shared_ptr<Gate> gate) : gate_(std::move(gate)) {}
        void on_progress(const ProgressRecord& /*record*/) override {
            std::unique_lock lock(gate_->mutex);
            gate_->entered = true;
            gate_->changed.notify_all();
            gate_->changed.wait(lock, [this] { return gate_->open; });
        }
        void flush() override {}

    private:
        std::shared_ptr<Gate> gate_;
    };

    auto gate = std::make_shared<Gate>();
    Core local{[](std::string_view /*line*/) {}};  // the refusal's diagnostic is expected
    ASSERT_TRUE(local.add_sink(std::make_unique<BlockingSink>(gate)).has_value());
    ProgressHandle bar(local, "items", progress_queue_capacity + 2);
    bar.tick();
    {
        std::unique_lock lock(gate->mutex);
        gate->changed.wait(lock, [&gate] { return gate->entered; });
    }
    for (std::size_t i = 0; i < progress_queue_capacity; ++i) {
        bar.tick();
    }
    ASSERT_EQ(local.statistics().progress.dropped_queue_full, 0U);
    EXPECT_EQ(allocations_during([&bar] { bar.tick(); }), 1U);
    EXPECT_EQ(local.statistics().progress.dropped_queue_full, 1U);
    {
        const std::scoped_lock lock(gate->mutex);
        gate->open = true;
    }
    gate->changed.notify_all();
    local.flush();
}

}  // namespace
}  // namespace lumen
