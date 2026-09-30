#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "lumen/core.h"
#include "lumen/detail/error.h"
#include "lumen/predicate.h"
#include "lumen/record.h"
#include "lumen/sink.h"

namespace lumen {
namespace {

struct CaptureSink : public Sink {
    std::vector<LogRecord> logs;
    std::vector<MetricRecord> metrics;
    std::vector<ProgressRecord> progress;
    std::mutex mtx;

    void on_log(const LogRecord& r) override {
        std::lock_guard lock(mtx);
        logs.push_back(r);
    }
    void on_metric(const MetricRecord& r) override {
        std::lock_guard lock(mtx);
        metrics.push_back(r);
    }
    void on_progress(const ProgressRecord& r) override {
        std::lock_guard lock(mtx);
        progress.push_back(r);
    }
    void flush() override {}
};

struct OrderSink : public Sink {
    std::vector<std::string> order;
    std::mutex mtx;

    void on_log(const LogRecord&) override {
        std::lock_guard lock(mtx);
        order.push_back("log");
    }
    void on_metric(const MetricRecord&) override {
        std::lock_guard lock(mtx);
        order.push_back("metric");
    }
    void on_progress(const ProgressRecord&) override {
        std::lock_guard lock(mtx);
        order.push_back("progress");
    }
    void flush() override {}
};

void wait_dispatch(int ms = 100) {
    core().flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// ── Fixture to manage sinks per test ──────────────────────────────────────────

class DispatchTest : public ::testing::Test {
protected:
    std::vector<SinkId> __ids;

    void TearDown() override {
        for (auto id : __ids) {
            (void)core().remove_sink(id);
        }
        __ids.clear();
    }

    SinkId register_sink(std::unique_ptr<Sink> sink, Predicate pred) {
        SinkId id = core().add_sink(std::move(sink), std::move(pred));
        __ids.push_back(id);
        return id;
    }
};

// ── Record routing ────────────────────────────────────────────────────────────

TEST_F(DispatchTest, RecordRoutedToCorrectSink) {
    auto sink1 = std::make_unique<CaptureSink>();
    auto* raw1 = sink1.get();
    register_sink(std::move(sink1), tag_equals("route", "1"));

    auto sink2 = std::make_unique<CaptureSink>();
    auto* raw2 = sink2.get();
    register_sink(std::move(sink2), tag_equals("route", "2"));

    // Emit a log with tag route=1 — only sink1 should see it
    {
        RecordBuilder builder(core().log_buffer(), LogLevel::INFO, "test msg");
        builder.tag("route", "1");
    }

    wait_dispatch();

    EXPECT_EQ(raw1->logs.size(), 1);
    EXPECT_EQ(raw1->logs[0].message, "test msg");
    EXPECT_EQ(raw2->logs.size(), 0);
}

TEST_F(DispatchTest, RecordRoutedToMultipleSinks) {
    auto sink1 = std::make_unique<CaptureSink>();
    auto* raw1 = sink1.get();
    register_sink(std::move(sink1), level_at_least(LogLevel::INFO));

    auto sink2 = std::make_unique<CaptureSink>();
    auto* raw2 = sink2.get();
    register_sink(std::move(sink2), always());

    {
        RecordBuilder builder(core().log_buffer(), LogLevel::INFO, "broadcast");
    }

    wait_dispatch();

    EXPECT_EQ(raw1->logs.size(), 1);
    EXPECT_EQ(raw2->logs.size(), 1);
}

TEST_F(DispatchTest, RecordRoutedToNoSink) {
    auto sink = std::make_unique<CaptureSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), tag_equals("env", "missing"));

    {
        RecordBuilder builder(core().log_buffer(), LogLevel::INFO, "no match");
    }

    wait_dispatch();

    EXPECT_EQ(raw->logs.size(), 0);
}

// ── Context merge priority ────────────────────────────────────────────────────

TEST_F(DispatchTest, ContextMergePriorityExplicitOverProcess) {
    set_process_tag("env", "prod");

    auto sink = std::make_unique<CaptureSink>();
    auto* raw = sink.get();

    // Sink only matches when env=staging (explicit tag overrides process tag)
    register_sink(std::move(sink), tag_equals("env", "staging"));

    {
        RecordBuilder builder(core().log_buffer(), LogLevel::INFO, "staging msg");
        builder.tag("env", "staging");
    }

    wait_dispatch();

    EXPECT_EQ(raw->logs.size(), 1);
    EXPECT_EQ(raw->logs[0].message, "staging msg");

    // Verify explicit tag is present (find returns first match = explicit)
    EXPECT_EQ(raw->logs[0].tags.find("env"), "staging");
}

TEST_F(DispatchTest, ProcessTagsAttached) {
    set_process_tag("service", "lumen-test");

    auto sink = std::make_unique<CaptureSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), tag_equals("service", "lumen-test"));

    {
        RecordBuilder builder(core().log_buffer(), LogLevel::INFO, "with proc tags");
    }

    wait_dispatch();

    EXPECT_GE(raw->logs.size(), 1);

    bool found = false;
    for (const auto& entry : raw->logs[0].tags) {
        if (entry.key == "service" && entry.value == "lumen-test") {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(DispatchTest, ThreadTagsAttached) {
    set_thread_tag("thread_name", "test-thread");

    auto sink = std::make_unique<CaptureSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), tag_equals("thread_name", "test-thread"));

    {
        RecordBuilder builder(core().log_buffer(), LogLevel::INFO, "with thread tags");
    }

    wait_dispatch();

    EXPECT_GE(raw->logs.size(), 1);

    bool found = false;
    for (const auto& entry : raw->logs[0].tags) {
        if (entry.key == "thread_name" && entry.value == "test-thread") {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

// ── Dispatch order (Progress → Metric → Log) ──────────────────────────────────

TEST_F(DispatchTest, DispatchOrder) {
    auto sink = std::make_unique<OrderSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), always());

    // Emit all three record types in quick succession
    {
        ProgressHandle ph(core().progress_buffer(), "progress_test", 100);
        ph.update(50);
        ph.finish();
    }

    core().emit(MetricRecord{.name = "test_metric", .value = 42.0, .timestamp_ns = 0});

    {
        RecordBuilder builder(core().log_buffer(), LogLevel::INFO, "order test log");
    }

    wait_dispatch();

    // Within a single drain cycle: Progress, then Metric, then Log
    ASSERT_GE(raw->order.size(), 3);

    // Find first occurrence of each type
    size_t first_progress = std::string::npos;
    size_t first_metric = std::string::npos;
    size_t first_log = std::string::npos;

    for (size_t i = 0; i < raw->order.size(); ++i) {
        if (raw->order[i] == "progress" && first_progress == std::string::npos)
            first_progress = i;
        if (raw->order[i] == "metric" && first_metric == std::string::npos)
            first_metric = i;
        if (raw->order[i] == "log" && first_log == std::string::npos)
            first_log = i;
    }

    EXPECT_NE(first_progress, std::string::npos);
    EXPECT_NE(first_metric, std::string::npos);
    EXPECT_NE(first_log, std::string::npos);

    EXPECT_LT(first_progress, first_metric);
    EXPECT_LT(first_metric, first_log);
}

// ── Metric and progress dispatch ──────────────────────────────────────────────

TEST_F(DispatchTest, MetricRecordDispatched) {
    auto sink = std::make_unique<CaptureSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), always());

    core().emit(MetricRecord{.name = "cpu", .value = 0.85, .timestamp_ns = 0});

    wait_dispatch();

    EXPECT_GE(raw->metrics.size(), 1);
    EXPECT_EQ(raw->metrics[0].name, "cpu");
    EXPECT_DOUBLE_EQ(raw->metrics[0].value, 0.85);
}

TEST_F(DispatchTest, ProgressRecordDispatched) {
    auto sink = std::make_unique<CaptureSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), always());

    core().emit(ProgressRecord{.label = "upload", .current = 50, .total = 100, .timestamp_ns = 0});

    wait_dispatch();

    EXPECT_GE(raw->progress.size(), 1);
    EXPECT_EQ(raw->progress[0].label, "upload");
    EXPECT_EQ(raw->progress[0].current, 50);
    EXPECT_EQ(raw->progress[0].total, 100);
}

// ── add_sink / remove_sink ────────────────────────────────────────────────────

TEST_F(DispatchTest, AddAndRemoveSink) {
    auto sink = std::make_unique<CaptureSink>();
    auto* raw = sink.get();
    SinkId id = core().add_sink(std::move(sink), always());

    {
        RecordBuilder builder(core().log_buffer(), LogLevel::INFO, "before remove");
    }
    wait_dispatch();
    EXPECT_EQ(raw->logs.size(), 1);

    auto removed = core().remove_sink(id);
    EXPECT_TRUE(removed.has_value());

    {
        RecordBuilder builder(core().log_buffer(), LogLevel::INFO, "after remove");
    }
    wait_dispatch();
    // sink was removed — should not receive more records
    EXPECT_EQ(raw->logs.size(), 1);
}

TEST_F(DispatchTest, RemoveSinkReturnsNullForUnknownId) {
    auto removed = core().remove_sink(99999);
    EXPECT_FALSE(removed.has_value());
}

// ── Level-based filtering ─────────────────────────────────────────────────────

TEST_F(DispatchTest, LevelFilteringSink) {
    auto sink = std::make_unique<CaptureSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), level_at_least(LogLevel::WARN));

    {
        RecordBuilder builder(core().log_buffer(), LogLevel::INFO, "should be filtered");
    }
    {
        RecordBuilder builder(core().log_buffer(), LogLevel::WARN, "should pass");
    }
    {
        RecordBuilder builder(core().log_buffer(), LogLevel::ERROR, "should also pass");
    }

    wait_dispatch();

    EXPECT_EQ(raw->logs.size(), 2);
}

// ── Thread tag isolation ──────────────────────────────────────────────────────

TEST_F(DispatchTest, ThreadTagsNotSharedBetweenThreads) {
    set_thread_tag("owner", "main");

    auto sink = std::make_unique<CaptureSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), tag_exists("owner"));

    // Emit from a different thread that has no thread tags
    std::thread other([this]() {
        {
            RecordBuilder builder(core().log_buffer(), LogLevel::INFO, "from other thread");
        }
    });

    other.join();
    wait_dispatch();

    // The record from the other thread should NOT have the "owner" tag
    // So it won't match our sink
    // But we should verify: the main thread tag doesn't leak to other threads
    bool has_owner_from_other = false;
    for (const auto& log : raw->logs) {
        if (log.message == "from other thread" && !log.tags.find("owner").empty()) {
            has_owner_from_other = true;
        }
    }
    EXPECT_FALSE(has_owner_from_other);
}

// ── Error-path tests ──────────────────────────────────────────────────────────

TEST_F(DispatchTest, EmptyTagSetDispatch) {
    auto sink = std::make_unique<CaptureSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), tag_exists("nonexistent"));

    {
        RecordBuilder builder(core().log_buffer(), LogLevel::INFO, "no tags");
    }

    wait_dispatch();

    EXPECT_EQ(raw->logs.size(), 0);
}

TEST_F(DispatchTest, RemoveAllSinksThenEmit) {
    auto sink = std::make_unique<CaptureSink>();
    SinkId id = core().add_sink(std::move(sink), always());
    (void)core().remove_sink(id);

    {
        RecordBuilder builder(core().log_buffer(), LogLevel::INFO, "after removal");
    }

    wait_dispatch();
    SUCCEED();
}

TEST_F(DispatchTest, MultipleRemoveSameSink) {
    auto sink = std::make_unique<CaptureSink>();
    SinkId id = core().add_sink(std::move(sink), always());

    auto removed1 = core().remove_sink(id);
    EXPECT_TRUE(removed1.has_value());

    auto removed2 = core().remove_sink(id);
    EXPECT_FALSE(removed2.has_value());
}

}  // namespace

// Test that runs outside the fixture (no sink cleanup needed)
namespace {

TEST(SingletonTest, CoreIsSingleton) {
    Core& c1 = core();
    Core& c2 = core();
    EXPECT_EQ(&c1, &c2);
}

// ── flush() semantics ────────────────────────────────────────────────────────

TEST_F(DispatchTest, FlushDeliversEverythingEmittedBeforeIt) {
    auto sink = std::make_unique<CaptureSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), always());

    for (int i = 0; i < 500; ++i) {
        LOG_INFO("record");
        lumen::metric(std::string("metric_name_longer_than_sso_") + std::to_string(i), 1.0);
    }
    core().flush();  // no sleep afterwards: flush itself must block

    std::lock_guard lock(raw->mtx);
    EXPECT_EQ(raw->logs.size(), 500u);
    ASSERT_EQ(raw->metrics.size(), 500u);
    EXPECT_EQ(raw->metrics[499].name, "metric_name_longer_than_sso_499");
}

TEST_F(DispatchTest, FlushWaitsForSinkFlush) {
    struct SlowFlushSink : public Sink {
        std::atomic<bool> flushed{false};
        void flush() override {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            flushed.store(true);
        }
    };
    auto sink = std::make_unique<SlowFlushSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), always());

    core().flush();
    EXPECT_TRUE(raw->flushed.load());
}

TEST_F(DispatchTest, FlushFromSinkCallbackDoesNotDeadlock) {
    struct ReentrantSink : public Sink {
        std::atomic<int> calls{0};
        void on_log(const LogRecord&) override {
            core().flush();
            ++calls;
        }
        void flush() override {}
    };
    auto sink = std::make_unique<ReentrantSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), always());

    LOG_INFO("reentrant");
    core().flush();
    EXPECT_EQ(raw->calls.load(), 1);
}

TEST_F(DispatchTest, FlushWithConcurrentProducers) {
    auto sink = std::make_unique<CaptureSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), always());

    constexpr int kThreads = 4;
    constexpr int kPerThread = 1000;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([t] {
            for (int i = 0; i < kPerThread; ++i) {
                LOG_INFO("t{} i{}", t, i).tag("thread", int64_t{t});
            }
            core().flush();
        });
    }
    for (auto& th : threads) th.join();
    core().flush();

    std::lock_guard lock(raw->mtx);
    EXPECT_EQ(raw->logs.size(), static_cast<size_t>(kThreads * kPerThread));
}

TEST_F(DispatchTest, ProgressLabelFromTemporaryIsCopied) {
    auto sink = std::make_unique<CaptureSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), always());

    {
        auto handle = lumen::progress(std::string(40, 'p'), 10);
        handle.update(3);
        handle.finish();
    }
    core().flush();

    std::lock_guard lock(raw->mtx);
    ASSERT_EQ(raw->progress.size(), 2u);
    EXPECT_EQ(raw->progress[0].label, std::string(40, 'p'));
    EXPECT_EQ(raw->progress[0].current, 3u);
    EXPECT_EQ(raw->progress[1].current, 10u);
    EXPECT_EQ(raw->progress[1].tags.count(), raw->progress[0].tags.count());
}

// ── Throwing sinks ───────────────────────────────────────────────────────────

TEST_F(DispatchTest, ThrowingSinkIsContainedAndOtherSinksStillReceive) {
    struct ThrowingSink : public Sink {
        void on_log(const LogRecord&) override { throw std::runtime_error("disk on fire"); }
        void flush() override { throw 42; }  // not even a std::exception
    };
    const SinkId bad = register_sink(std::make_unique<ThrowingSink>(), always());
    auto good = std::make_unique<CaptureSink>();
    auto* raw = good.get();
    register_sink(std::move(good), always());

    LOG_INFO("first");
    LOG_INFO("second");
    EXPECT_NO_THROW(core().flush());

    {
        std::lock_guard lock(raw->mtx);
        EXPECT_EQ(raw->logs.size(), 2u);
    }
    const auto count = core().sink_exception_count(bad);
    ASSERT_TRUE(count.has_value());
    EXPECT_EQ(*count, 3u);  // two on_log + one flush
}

TEST_F(DispatchTest, SinkExceptionCountForUnknownSinkIsError) {
    const auto count = core().sink_exception_count(SinkId{0});
    ASSERT_FALSE(count.has_value());
    EXPECT_EQ(count.error(), make_error_code(LumenError::invalid_sink_id));
}

// ── Level filters and records without a level ────────────────────────────────

TEST_F(DispatchTest, LevelFilteredSinkReceivesAllMetricsAndProgress) {
    auto sink = std::make_unique<CaptureSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), level_at_least(LogLevel::WARN));

    LOG_INFO("filtered out");
    LOG_ERROR("passes");
    lumen::metric("altitude", 69999.97);
    {
        auto handle = lumen::progress("burn", 2);
        handle.finish();
    }
    core().flush();

    std::lock_guard lock(raw->mtx);
    EXPECT_EQ(raw->logs.size(), 1u);
    EXPECT_EQ(raw->metrics.size(), 1u);
    EXPECT_EQ(raw->progress.size(), 1u);
}

TEST_F(DispatchTest, ParsedQueryFiltersMetricsByTagsOnly) {
    auto parsed = parse_predicate("level >= WARN && vessel_id == 42");
    ASSERT_TRUE(parsed.has_value());
    auto sink = std::make_unique<CaptureSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), std::move(*parsed));

    lumen::metric("thrust", 1.0).tag("vessel_id", 42);
    lumen::metric("thrust", 2.0).tag("vessel_id", 7);
    LOG_INFO("wrong level").tag("vessel_id", 42);
    LOG_WARN("right level").tag("vessel_id", 42);
    core().flush();

    std::lock_guard lock(raw->mtx);
    ASSERT_EQ(raw->metrics.size(), 1u);
    EXPECT_DOUBLE_EQ(raw->metrics[0].value, 1.0);
    ASSERT_EQ(raw->logs.size(), 1u);
    EXPECT_EQ(raw->logs[0].message, "right level");
}

}  // namespace
}  // namespace lumen
