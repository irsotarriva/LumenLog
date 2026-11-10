// Core: submission, delivery, the sink registry, statistics, diagnostics,
// shutdown and fatal records. Every test here runs against its own Core.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <source_location>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "lumen/core.hpp"
#include "lumen/error.hpp"
#include "lumen/limits.hpp"
#include "lumen/predicate.hpp"
#include "lumen/progress.hpp"
#include "lumen/record_builder.hpp"
#include "lumen/thread_clock.hpp"
#include "test_support.hpp"

namespace lumen {
namespace {

using test::capture;
using test::Captured;
using test::CaptureSink;
using test::LocalCoreTest;
using test::log_to;
using test::tag_of;

constexpr std::chrono::seconds generous_timeout{10};

// A gate a sink's callback blocks on until the test opens it.
class Gate {
public:
    void wait_inside() {
        std::unique_lock lock(mutex_);
        ++entered_;
        changed_.notify_all();
        changed_.wait(lock, [this] { return open_; });
    }
    [[nodiscard]] bool wait_until_entered(int count) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, generous_timeout, [&] { return entered_ >= count; });
    }
    void open() {
        {
            const std::scoped_lock lock(mutex_);
            open_ = true;
        }
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;  // waits on mutex_
    int entered_ = 0;                  // guarded by mutex_
    bool open_ = false;                // guarded by mutex_
};

// Blocks inside on_log until the gate opens; counts what it received.
class GateSink : public Sink {
public:
    GateSink(std::shared_ptr<Gate> gate, std::shared_ptr<std::atomic<uint64_t>> received)
        : gate_(std::move(gate)), received_(std::move(received)) {}
    void on_log(const LogRecord&) override {
        gate_->wait_inside();
        received_->fetch_add(1);
    }
    void flush() override {}

private:
    std::shared_ptr<Gate> gate_;
    std::shared_ptr<std::atomic<uint64_t>> received_;
};

// Calls `action` from inside on_log, once per record.
template <typename F>
class CallbackSink : public Sink {
public:
    explicit CallbackSink(F action) : action_(std::move(action)) {}
    void on_log(const LogRecord& r) override { action_(r); }
    void flush() override {}

private:
    F action_;
};

template <typename F>
[[nodiscard]] std::unique_ptr<Sink> callback_sink(F action) {
    return std::make_unique<CallbackSink<F>>(std::move(action));
}

// ── Submission: every record is complete ─────────────────────────────────

class CoreSubmitTest : public LocalCoreTest {};

TEST_F(CoreSubmitTest, LogRecordCarriesTimestampThreadIdSourceAndItsOwnMessage) {
    const uint64_t before = now_ns();
    const std::source_location here = std::source_location::current();
    {
        std::string message =
            "message from a temporary string, longer than any small-string buffer";
        RecordBuilder(core_, LogLevel::Warn, here, message);
        message.assign(message.size(), 'x');  // the record must not see this
    }
    const uint64_t after = now_ns();
    core_.flush();

    const auto logs = captured_->logs();
    ASSERT_EQ(logs.size(), 1U);
    const LogRecord& r = logs[0];
    EXPECT_EQ(r.message, "message from a temporary string, longer than any small-string buffer");
    EXPECT_EQ(r.level, LogLevel::Warn);
    EXPECT_GE(r.timestamp_ns, before);
    EXPECT_LE(r.timestamp_ns, after);
    EXPECT_EQ(r.thread_id, this_thread_id());
    EXPECT_EQ(r.source.line, here.line());
    EXPECT_EQ(r.source.file, here.file_name());
    ASSERT_NE(r.storage, nullptr);
    EXPECT_EQ(r.message.data(), r.storage.get());  // the message lives in the record's block
}

TEST_F(CoreSubmitTest, MetricAndProgressRecordsCarryTimestampAndThreadId) {
    const uint64_t before = now_ns();
    MetricBuilder(core_, "cpu", 0.85);
    {
        ProgressHandle bar(core_, "upload", 100);
        bar.update(50);
    }
    core_.flush();

    const auto metrics = captured_->metrics();
    const auto progress = captured_->progress();
    ASSERT_EQ(metrics.size(), 1U);
    ASSERT_EQ(progress.size(), 1U);
    EXPECT_EQ(metrics[0].name, "cpu");
    EXPECT_DOUBLE_EQ(metrics[0].value, 0.85);
    EXPECT_GE(metrics[0].timestamp_ns, before);
    EXPECT_EQ(metrics[0].thread_id, this_thread_id());
    EXPECT_EQ(progress[0].label, "upload");
    EXPECT_EQ(progress[0].current, 50U);
    EXPECT_GE(progress[0].timestamp_ns, before);
    EXPECT_EQ(progress[0].thread_id, this_thread_id());
}

TEST_F(CoreSubmitTest, RecordsCarryTheProducingThreadsContext) {
    ASSERT_TRUE(set_process_tag("app", "test"));
    std::thread producer([this] {
        ASSERT_TRUE(set_thread_tag("worker", "7"));
        log_to(core_, "from worker");
        MetricBuilder(core_, "m", 1.0);
        ProgressHandle(core_, "p", 1).finish();
        clear_thread_tags();
    });
    producer.join();
    core_.flush();

    const auto logs = captured_->logs();
    const auto metrics = captured_->metrics();
    const auto progress = captured_->progress();
    ASSERT_EQ(logs.size(), 1U);
    ASSERT_EQ(metrics.size(), 1U);
    ASSERT_EQ(progress.size(), 1U);
    for (const RecordTags* tags : {&logs[0].tags, &metrics[0].tags, &progress[0].tags}) {
        EXPECT_EQ(tag_of(*tags, "worker"), "7");
        EXPECT_EQ(tag_of(*tags, "app"), "test");
    }
}

TEST_F(CoreSubmitTest, RecordIsDeliveredWithoutFlushWhileTheDispatcherIdles) {
    // Long enough for an idle dispatcher to be asleep, so this checks that
    // submission wakes it.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    log_to(core_, "wake up");
    EXPECT_TRUE(captured_->wait_for_logs(1, generous_timeout));
}

TEST_F(CoreSubmitTest, ThousandRecordsFromOneThreadArriveInSubmissionOrder) {
    for (int i = 0; i < 1000; ++i)
        log_to(core_, "ordered").tag("i", i);
    core_.flush();
    const auto logs = captured_->logs();
    ASSERT_EQ(logs.size(), 1000U);
    for (std::size_t i = 0; i < logs.size(); ++i) {
        EXPECT_EQ(tag_of(logs[i].tags, "i"), std::to_string(i));
    }
}

TEST_F(CoreSubmitTest, OnePassDeliversProgressThenMetricsThenLogs) {
    auto gate = std::make_shared<Gate>();
    auto received = std::make_shared<std::atomic<uint64_t>>(0);
    const auto gate_id =
        core_.add_sink(std::make_unique<GateSink>(gate, received), tag_exists("block"));
    ASSERT_TRUE(gate_id);
    log_to(core_, "blocker").tag("block", true);
    ASSERT_TRUE(gate->wait_until_entered(1));

    // Queued while the dispatcher is blocked, so the next pass finds all three.
    log_to(core_, "log");
    MetricBuilder(core_, "metric", 1.0);
    ProgressHandle(core_, "progress", 1).finish();

    struct OrderSink : Sink {
        explicit OrderSink(std::shared_ptr<std::vector<std::string>> out) : order(std::move(out)) {}
        void on_log(const LogRecord& r) override {
            if (r.message == "log") order->emplace_back("log");
        }
        void on_metric(const MetricRecord&) override { order->emplace_back("metric"); }
        void on_progress(const ProgressRecord&) override { order->emplace_back("progress"); }
        void flush() override {}
        std::shared_ptr<std::vector<std::string>> order;  // written on the delivering thread only
    };
    auto order = std::make_shared<std::vector<std::string>>();
    ASSERT_TRUE(core_.add_sink(std::make_unique<OrderSink>(order)));
    gate->open();
    core_.flush();
    EXPECT_EQ(*order, (std::vector<std::string>{"progress", "metric", "log"}));
}

// ── Routing ──────────────────────────────────────────────────────────────

class CoreRoutingTest : public LocalCoreTest {};

TEST_F(CoreRoutingTest, EachSinkReceivesTheLogRecordsItsPredicateAccepts) {
    const auto warnings = capture(core_, level_at_least(LogLevel::Warn));
    const auto tagged = capture(core_, tag_equals("route", "a"));
    log_to(core_, "info a", LogLevel::Info).tag("route", "a");
    log_to(core_, "error b", LogLevel::Error).tag("route", "b");
    log_to(core_, "info", LogLevel::Info);
    core_.flush();

    EXPECT_EQ(captured_->messages(), (std::vector<std::string>{"info a", "error b", "info"}));
    EXPECT_EQ(warnings->messages(), (std::vector<std::string>{"error b"}));
    EXPECT_EQ(tagged->messages(), (std::vector<std::string>{"info a"}));
}

TEST_F(CoreRoutingTest, MetricsAndProgressAreFilteredByTagsAndIgnoreLevelConditions) {
    auto parsed = parse_predicate("level >= WARN && unit == 42");
    ASSERT_TRUE(parsed.has_value());
    const auto filtered = capture(core_, std::move(*parsed));
    MetricBuilder(core_, "thrust", 1.0).tag("unit", 42);
    MetricBuilder(core_, "thrust", 2.0).tag("unit", 7);
    log_to(core_, "wrong level", LogLevel::Info).tag("unit", 42);
    log_to(core_, "right level", LogLevel::Warn).tag("unit", 42);
    core_.flush();

    const auto metrics = filtered->metrics();
    ASSERT_EQ(metrics.size(), 1U);
    EXPECT_DOUBLE_EQ(metrics[0].value, 1.0);
    EXPECT_EQ(filtered->messages(), (std::vector<std::string>{"right level"}));
}

// ── flush() ──────────────────────────────────────────────────────────────

class CoreFlushTest : public LocalCoreTest {};

TEST_F(CoreFlushTest, FlushDeliversEverySubmittedRecordAndFlushesEachSinkBeforeReturning) {
    for (int i = 0; i < 500; ++i) {
        log_to(core_, "record");
        MetricBuilder(core_, "metric_name_longer_than_small_string_" + std::to_string(i), 1.0);
    }
    const std::size_t flushes_before = captured_->flushes();
    core_.flush();
    EXPECT_EQ(captured_->logs().size(), 500U);
    const auto metrics = captured_->metrics();
    ASSERT_EQ(metrics.size(), 500U);
    EXPECT_EQ(metrics[499].name, "metric_name_longer_than_small_string_499");
    EXPECT_EQ(captured_->flushes(), flushes_before + 1);
}

TEST_F(CoreFlushTest, FlushFromInsideACallbackReturnsAtOnceWithoutFlushingSinks) {
    auto flushes_seen_inside = std::make_shared<std::vector<std::size_t>>();
    const std::shared_ptr<Captured> captured = captured_;
    Core& core = core_;
    ASSERT_TRUE(
        core_.add_sink(callback_sink([&core, captured, flushes_seen_inside](const LogRecord&) {
            const std::size_t before = captured->flushes();
            core.flush();
            flushes_seen_inside->push_back(before);
            flushes_seen_inside->push_back(captured->flushes());
        })));
    log_to(core_, "trigger");
    core_.flush();

    ASSERT_EQ(flushes_seen_inside->size(), 2U);
    EXPECT_EQ((*flushes_seen_inside)[0], (*flushes_seen_inside)[1]);
    EXPECT_EQ(captured_->logs().size(), 1U);
}

TEST_F(CoreFlushTest, FlushOnEachOfFourProducerThreadsLeavesNoRecordUndelivered) {
    constexpr int thread_count = 4;
    constexpr int per_thread = 1000;
    std::vector<std::thread> threads;
    for (int t = 0; t < thread_count; ++t) {
        threads.emplace_back([this, t] {
            for (int i = 0; i < per_thread; ++i)
                log_to(core_, "concurrent").tag("t", t);
            core_.flush();
        });
    }
    for (std::thread& t : threads)
        t.join();
    EXPECT_EQ(captured_->logs().size(), static_cast<std::size_t>(thread_count * per_thread));
}

// ── Loss accounting ──────────────────────────────────────────────────────

class CoreStatisticsTest : public LocalCoreTest {};

TEST_F(CoreStatisticsTest, WithABlockedSinkDeliveredPlusDroppedEqualsEmittedExactly) {
    auto gate = std::make_shared<Gate>();
    auto received = std::make_shared<std::atomic<uint64_t>>(0);
    ASSERT_TRUE(core_.add_sink(std::make_unique<GateSink>(gate, received)));

    log_to(core_, "first");
    ASSERT_TRUE(gate->wait_until_entered(1));  // the dispatcher is now stuck in the sink
    constexpr uint64_t extra = 1000;
    const uint64_t emitted = 1 + log_queue_capacity + extra;
    for (uint64_t i = 1; i < emitted; ++i)
        log_to(core_, "flood");
    const CoreStatistics while_blocked = core_.statistics();
    gate->open();
    core_.flush();

    const CoreStatistics stats = core_.statistics();
    const uint64_t delivered = received->load();
    EXPECT_EQ(delivered + stats.logs.dropped_queue_full, emitted);
    EXPECT_EQ(stats.logs.accepted, delivered);
    EXPECT_EQ(stats.logs.dropped_queue_full, extra);
    EXPECT_EQ(while_blocked.logs.dropped_queue_full, extra);
    EXPECT_EQ(stats.logs.dropped_after_shutdown, 0U);
    EXPECT_EQ(captured_->logs().size(), delivered);
}

TEST_F(CoreStatisticsTest, FirstQueueFullDropIsReportedOnceAsADiagnosticLine) {
    auto gate = std::make_shared<Gate>();
    auto received = std::make_shared<std::atomic<uint64_t>>(0);
    ASSERT_TRUE(core_.add_sink(std::make_unique<GateSink>(gate, received)));
    log_to(core_, "first");
    ASSERT_TRUE(gate->wait_until_entered(1));
    for (uint64_t i = 0; i < log_queue_capacity + 10; ++i)
        log_to(core_, "flood");
    gate->open();
    core_.flush();
    for (int i = 0; i < 10; ++i)
        log_to(core_, "after");
    core_.flush();

    const auto lines = diagnostics();
    ASSERT_EQ(lines.size(), 1U);
    EXPECT_NE(lines[0].find("queue was full"), std::string::npos);
}

TEST_F(CoreStatisticsTest, StatisticsCountAcceptedRecordsPerType) {
    for (int i = 0; i < 3; ++i)
        log_to(core_, "l");
    for (int i = 0; i < 2; ++i)
        MetricBuilder(core_, "m", 1.0);
    ProgressHandle(core_, "p", 1).finish();
    core_.flush();
    const CoreStatistics stats = core_.statistics();
    EXPECT_EQ(stats.logs.accepted, 3U);
    EXPECT_EQ(stats.metrics.accepted, 2U);
    EXPECT_EQ(stats.progress.accepted, 1U);
    EXPECT_EQ(stats.logs.dropped_queue_full + stats.metrics.dropped_queue_full +
                  stats.progress.dropped_queue_full,
              0U);
    EXPECT_EQ(stats.tags_dropped, 0U);
    EXPECT_TRUE(diagnostics().empty());
}

TEST_F(CoreStatisticsTest, EmptyDiagnosticHandlerSilencesDiagnostics) {
    core_.set_diagnostic_handler({});
    auto builder_with_too_many_tags = [this] {
        RecordBuilder b(core_, LogLevel::Info, std::source_location::current(), "full");
        for (std::size_t i = 0; i <= max_record_tags; ++i)
            b.tag("k" + std::to_string(i), i);
    };
    builder_with_too_many_tags();
    core_.flush();
    EXPECT_EQ(core_.statistics().tags_dropped, 1U);
    EXPECT_TRUE(diagnostics().empty());
}

// ── The sink registry ────────────────────────────────────────────────────

class CoreRegistryTest : public LocalCoreTest {};

TEST_F(CoreRegistryTest, AddSinkRejectsANullSinkAndDeliveryContinues) {
    const auto id = core_.add_sink(nullptr);
    ASSERT_FALSE(id.has_value());
    EXPECT_EQ(id.error(), LumenError::NullSink);
    log_to(core_, "still delivered");
    core_.flush();
    EXPECT_EQ(captured_->logs().size(), 1U);
}

TEST_F(CoreRegistryTest, RemovedSinkIsReturnedAndReceivesNothingAfterwards) {
    auto mine = std::make_shared<Captured>();
    const auto id = core_.add_sink(std::make_unique<CaptureSink>(mine));
    ASSERT_TRUE(id);
    log_to(core_, "before");
    core_.flush();
    auto removed = core_.remove_sink(*id);
    ASSERT_TRUE(removed.has_value());
    EXPECT_NE(removed->get(), nullptr);
    log_to(core_, "after");
    core_.flush();
    EXPECT_EQ(mine->messages(), (std::vector<std::string>{"before"}));
    EXPECT_EQ(captured_->messages(), (std::vector<std::string>{"before", "after"}));
}

TEST_F(CoreRegistryTest, RemoveSinkFailsWithInvalidSinkIdForUnknownOrAlreadyRemovedIds) {
    const auto unknown = core_.remove_sink(99999);
    ASSERT_FALSE(unknown.has_value());
    EXPECT_EQ(unknown.error(), LumenError::InvalidSinkId);

    const auto id = core_.add_sink(std::make_unique<NullSink>());
    ASSERT_TRUE(id);
    EXPECT_TRUE(core_.remove_sink(*id).has_value());
    const auto again = core_.remove_sink(*id);
    ASSERT_FALSE(again.has_value());
    EXPECT_EQ(again.error(), LumenError::InvalidSinkId);
}

TEST_F(CoreRegistryTest, RemoveSinkWaitsForItsRunningCallbackAndQueuedRecordsAreNotDelivered) {
    auto gate = std::make_shared<Gate>();
    auto received = std::make_shared<std::atomic<uint64_t>>(0);
    const auto id = core_.add_sink(std::make_unique<GateSink>(gate, received));
    ASSERT_TRUE(id);
    for (int i = 0; i < 5; ++i)
        log_to(core_, "queued");
    ASSERT_TRUE(gate->wait_until_entered(1));

    std::atomic<bool> remove_returned{false};
    std::thread remover([&] {
        EXPECT_TRUE(core_.remove_sink(*id).has_value());
        remove_returned = true;
    });
    // remove_sink must still be waiting: the callback has not returned.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(remove_returned.load());
    EXPECT_EQ(received->load(), 0U);
    gate->open();
    remover.join();
    // The callback that was running completed before remove_sink returned...
    EXPECT_EQ(received->load(), 1U);
    core_.flush();
    // ...and the four records still queued never reached the removed sink.
    EXPECT_EQ(received->load(), 1U);
    EXPECT_EQ(captured_->logs().size(), 5U);
}

TEST_F(CoreRegistryTest, SinkCanAddRemoveAndQuerySinksFromInsideItsCallback) {
    const auto other = core_.add_sink(std::make_unique<NullSink>());
    ASSERT_TRUE(other);
    auto results = std::make_shared<std::vector<bool>>();
    Core& core = core_;
    const SinkId other_id = *other;
    ASSERT_TRUE(core_.add_sink(callback_sink([&core, other_id, results](const LogRecord& r) {
        if (r.message != "trigger") return;
        results->push_back(core.add_sink(std::make_unique<NullSink>()).has_value());
        results->push_back(core.sink_exception_count(other_id).has_value());
        results->push_back(core.remove_sink(other_id).has_value());
    })));
    log_to(core_, "trigger");
    core_.flush();
    EXPECT_EQ(*results, (std::vector<bool>{true, true, true}));
}

TEST_F(CoreRegistryTest, SinkRemovingItselfFromItsCallbackGetsSinkBusyAndStaysRegistered) {
    auto self_id = std::make_shared<std::atomic<SinkId>>(0);
    auto errors = std::make_shared<std::vector<std::error_code>>();
    auto calls = std::make_shared<std::atomic<int>>(0);
    Core& core = core_;
    const auto id = core_.add_sink(callback_sink([&core, self_id, errors, calls](const LogRecord&) {
        calls->fetch_add(1);
        const auto result = core.remove_sink(self_id->load());
        if (!result) errors->push_back(result.error());
    }));
    ASSERT_TRUE(id);
    self_id->store(*id);
    log_to(core_, "one");
    log_to(core_, "two");
    core_.flush();
    EXPECT_EQ(calls->load(), 2);  // still registered after the first attempt
    ASSERT_EQ(errors->size(), 2U);
    EXPECT_EQ((*errors)[0], LumenError::SinkBusy);
    EXPECT_TRUE(core_.remove_sink(*id).has_value());
}

// The shape of the Python deadlock: the registering thread holds a lock that
// the running callback needs (the GIL there, a mutex here).
TEST_F(CoreRegistryTest, AddSinkDoesNotWaitForARunningCallback) {
    auto shared_lock = std::make_shared<std::mutex>();
    auto started = std::make_shared<std::atomic<bool>>(false);
    ASSERT_TRUE(core_.add_sink(callback_sink([shared_lock, started](const LogRecord&) {
        started->store(true);
        const std::scoped_lock lock(*shared_lock);
    })));
    std::unique_lock held(*shared_lock);
    log_to(core_, "needs the lock");
    // Wait until the callback runs; it then blocks on the lock we hold. If
    // add_sink waited for callbacks, the calls below would never return.
    const auto deadline = std::chrono::steady_clock::now() + generous_timeout;
    while (!started->load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    ASSERT_TRUE(started->load());
    const auto added = core_.add_sink(std::make_unique<NullSink>());
    ASSERT_TRUE(added.has_value());
    EXPECT_TRUE(core_.sink_exception_count(*added).has_value());
    EXPECT_TRUE(core_.remove_sink(*added).has_value());
    held.unlock();
    core_.flush();
}

TEST_F(CoreRegistryTest, ExceptionsFromASinkAreCountedReportedOnceAndOtherSinksStillReceive) {
    struct ThrowingSink : Sink {
        void on_log(const LogRecord&) override { throw std::runtime_error("disk on fire"); }
        void flush() override { throw 42; }  // not a std::exception
    };
    const auto bad = core_.add_sink(std::make_unique<ThrowingSink>());
    ASSERT_TRUE(bad);
    log_to(core_, "first");
    log_to(core_, "second");
    core_.flush();

    EXPECT_EQ(captured_->logs().size(), 2U);
    const auto count = core_.sink_exception_count(*bad);
    ASSERT_TRUE(count.has_value());
    EXPECT_EQ(*count, 3U);  // two on_log, one flush
    const auto lines = diagnostics();
    ASSERT_EQ(lines.size(), 1U);
    EXPECT_NE(lines[0].find("disk on fire"), std::string::npos);
}

TEST_F(CoreRegistryTest, SinkExceptionCountForAnUnknownIdFailsWithInvalidSinkId) {
    const auto count = core_.sink_exception_count(SinkId{0});
    ASSERT_FALSE(count.has_value());
    EXPECT_EQ(count.error(), LumenError::InvalidSinkId);
}

// ── Shutdown ─────────────────────────────────────────────────────────────

TEST(CoreShutdownTest, DestroyingACoreDeliversQueuedRecordsFlushesAndDestroysItsSinks) {
    struct LifetimeSink : CaptureSink {
        LifetimeSink(std::shared_ptr<Captured> c, std::shared_ptr<std::atomic<bool>> d)
            : CaptureSink(std::move(c)), destroyed(std::move(d)) {}
        ~LifetimeSink() override { destroyed->store(true); }
        LifetimeSink(const LifetimeSink&) = delete;
        LifetimeSink& operator=(const LifetimeSink&) = delete;
        LifetimeSink(LifetimeSink&&) = delete;
        LifetimeSink& operator=(LifetimeSink&&) = delete;
        std::shared_ptr<std::atomic<bool>> destroyed;
    };
    const test::CleanContext context;
    auto captured = std::make_shared<Captured>();
    auto destroyed = std::make_shared<std::atomic<bool>>(false);
    {
        Core core;
        ASSERT_TRUE(core.add_sink(std::make_unique<LifetimeSink>(captured, destroyed)));
        for (int i = 0; i < 1000; ++i)
            log_to(core, "queued at destruction");
    }
    EXPECT_EQ(captured->logs().size(), 1000U);
    EXPECT_EQ(captured->flushes(), 1U);
    EXPECT_TRUE(destroyed->load());
}

// ── Fatal records ────────────────────────────────────────────────────────

// Writes what it receives to stderr, where the death test reads it.
class StderrSink : public Sink {
public:
    void on_log(const LogRecord& r) override { std::cerr << "delivered:" << r.message << '\n'; }
    void flush() override { std::cerr << "flushed" << std::endl; }
};

TEST(CoreFatalDeathTest,
     FatalRecordIsDeliveredAfterEarlierRecordsAndSinksAreFlushedBeforeTermination) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            Core core;
            static_cast<void>(core.add_sink(std::make_unique<StderrSink>()));
            log_to(core, "earlier");
            RecordBuilder(core, LogLevel::Fatal, std::source_location::current(), "boom");
        },
        "delivered:earlier\ndelivered:boom\nflushed");
}

TEST(CoreFatalDeathTest, FatalFromASinkCallbackReachesTheOtherSinksAndFlushesThem) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            Core core;
            Core* const target = &core;
            static_cast<void>(core.add_sink(callback_sink([target](const LogRecord& r) {
                if (r.message == "trigger") {
                    RecordBuilder(*target, LogLevel::Fatal, std::source_location::current(),
                                  "fatal from callback");
                }
            })));
            static_cast<void>(core.add_sink(std::make_unique<StderrSink>()));
            log_to(core, "trigger");
            core.flush();
        },
        // The sink that logged is skipped; the other one has not seen
        // "trigger" yet (it comes later in the list) and never will.
        "^delivered:fatal from callback\nflushed");
}

}  // namespace
}  // namespace lumen
