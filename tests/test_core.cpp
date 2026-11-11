// Core: submission, delivery, the sink registry, statistics, diagnostics,
// shutdown and fatal records. Every test here runs against its own Core.
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <expected>
#include <iostream>
#include <memory>
#include <mutex>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
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
using test::metric_to;
using test::StderrSink;
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
    void on_log(const LogRecord& /*record*/) override {
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

// Submission: every record is complete.

class CoreSubmitTest : public LocalCoreTest {};

TEST_F(CoreSubmitTest, LogRecordCarriesTimestampThreadIdSourceAndItsOwnMessage) {
    const uint64_t before = now_ns();
    const std::source_location here = std::source_location::current();
    {
        std::string message =
            "message from a temporary string, longer than any small-string buffer";
        log_to(_core, message, LogLevel::Warn, here);
        message.assign(message.size(), 'x');  // the record must not see this
    }
    const uint64_t after = now_ns();
    _core.flush();

    const auto logs = _captured->logs();
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
    metric_to(_core, "cpu", 0.85);
    {
        ProgressHandle bar(_core, "upload", 100);
        bar.update(50);
    }
    _core.flush();

    const auto metrics = _captured->metrics();
    const auto progress = _captured->progress();
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
        log_to(_core, "from worker");
        metric_to(_core, "m", 1.0);
        ProgressHandle(_core, "p", 1).finish();
        clear_thread_tags();
    });
    producer.join();
    _core.flush();

    const auto logs = _captured->logs();
    const auto metrics = _captured->metrics();
    const auto progress = _captured->progress();
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
    log_to(_core, "wake up");
    EXPECT_TRUE(_captured->wait_for_logs(1, generous_timeout));
}

// std::clock() is the process's CPU time on POSIX systems but wall time on
// Windows, where this measurement would be meaningless.
#ifndef PLATFORM_WINDOWS
TEST_F(CoreSubmitTest, IdleDispatcherUsesUnderATenthOfTheCpuTimeOfABusyLoop) {
    log_to(_core, "last record before idling");
    _core.flush();
    const std::clock_t cpu_before = std::clock();
    // Not a wait for anything: the interval over which the idle process's
    // CPU time is measured. A dispatcher that never slept would spend all
    // of it on a core; an idle one spends almost none.
    constexpr std::chrono::milliseconds idle_interval{500};
    std::this_thread::sleep_for(idle_interval);
    const std::clock_t cpu_used = std::clock() - cpu_before;
    const auto busy_loop_cpu = static_cast<std::clock_t>(
        std::chrono::duration<double>(idle_interval).count() * CLOCKS_PER_SEC);
    EXPECT_LT(cpu_used, busy_loop_cpu / 10);
}
#endif

TEST_F(CoreSubmitTest, ThousandRecordsFromOneThreadArriveInSubmissionOrder) {
    for (int i = 0; i < 1000; ++i) {
        log_to(_core, "ordered").tag("i", i);
    }
    _core.flush();
    const auto logs = _captured->logs();
    ASSERT_EQ(logs.size(), 1000U);
    for (std::size_t i = 0; i < logs.size(); ++i) {
        EXPECT_EQ(tag_of(logs[i].tags, "i"), std::to_string(i));
    }
}

TEST_F(CoreSubmitTest, OnePassDeliversProgressThenMetricsThenLogs) {
    auto gate = std::make_shared<Gate>();
    auto received = std::make_shared<std::atomic<uint64_t>>(0);
    const auto gate_id =
        _core.add_sink(std::make_unique<GateSink>(gate, received), tag_exists("block"));
    ASSERT_TRUE(gate_id);
    log_to(_core, "blocker").tag("block", true);
    ASSERT_TRUE(gate->wait_until_entered(1));

    // Queued while the dispatcher is blocked, so the next pass finds all three.
    log_to(_core, "log");
    metric_to(_core, "metric", 1.0);
    ProgressHandle(_core, "progress", 1).finish();

    struct OrderSink : Sink {
        explicit OrderSink(std::shared_ptr<std::vector<std::string>> out) : order(std::move(out)) {}
        void on_log(const LogRecord& r) override {
            if (r.message == "log") {
                order->emplace_back("log");
            }
        }
        void on_metric(const MetricRecord& /*record*/) override { order->emplace_back("metric"); }
        void on_progress(const ProgressRecord& /*record*/) override {
            order->emplace_back("progress");
        }
        void flush() override {}
        std::shared_ptr<std::vector<std::string>> order;  // written on the delivering thread only
    };
    auto order = std::make_shared<std::vector<std::string>>();
    ASSERT_TRUE(_core.add_sink(std::make_unique<OrderSink>(order)));
    gate->open();
    _core.flush();
    EXPECT_EQ(*order, (std::vector<std::string>{"progress", "metric", "log"}));
}

// Routing.

class CoreRoutingTest : public LocalCoreTest {};

TEST_F(CoreRoutingTest, EachSinkReceivesTheLogRecordsItsPredicateAccepts) {
    const auto warnings = capture(_core, level_at_least(LogLevel::Warn));
    const auto tagged = capture(_core, tag_equals("route", "a"));
    log_to(_core, "info a", LogLevel::Info).tag("route", "a");
    log_to(_core, "error b", LogLevel::Error).tag("route", "b");
    log_to(_core, "info", LogLevel::Info);
    _core.flush();

    EXPECT_EQ(_captured->messages(), (std::vector<std::string>{"info a", "error b", "info"}));
    EXPECT_EQ(warnings->messages(), (std::vector<std::string>{"error b"}));
    EXPECT_EQ(tagged->messages(), (std::vector<std::string>{"info a"}));
}

TEST_F(CoreRoutingTest, MetricsAndProgressAreFilteredByTagsAndIgnoreLevelConditions) {
    auto parsed = parse_predicate("level >= WARN && unit == 42");
    ASSERT_TRUE(parsed.has_value());
    const auto filtered = capture(_core, std::move(*parsed));
    metric_to(_core, "thrust", 1.0).tag("unit", 42);
    metric_to(_core, "thrust", 2.0).tag("unit", 7);
    log_to(_core, "wrong level", LogLevel::Info).tag("unit", 42);
    log_to(_core, "right level", LogLevel::Warn).tag("unit", 42);
    _core.flush();

    const auto metrics = filtered->metrics();
    ASSERT_EQ(metrics.size(), 1U);
    EXPECT_DOUBLE_EQ(metrics[0].value, 1.0);
    EXPECT_EQ(filtered->messages(), (std::vector<std::string>{"right level"}));
}

// flush().

class CoreFlushTest : public LocalCoreTest {};

TEST_F(CoreFlushTest, FlushDeliversEverySubmittedRecordAndFlushesEachSinkBeforeReturning) {
    for (int i = 0; i < 500; ++i) {
        log_to(_core, "record");
        metric_to(_core, "metric_name_longer_than_small_string_" + std::to_string(i), 1.0);
    }
    const std::size_t flushes_before = _captured->flushes();
    _core.flush();
    EXPECT_EQ(_captured->logs().size(), 500U);
    const auto metrics = _captured->metrics();
    ASSERT_EQ(metrics.size(), 500U);
    EXPECT_EQ(metrics[499].name, "metric_name_longer_than_small_string_499");
    EXPECT_EQ(_captured->flushes(), flushes_before + 1);
}

TEST_F(CoreFlushTest, FlushFromInsideACallbackReturnsAtOnceWithoutFlushingSinks) {
    auto flushes_seen_inside = std::make_shared<std::vector<std::size_t>>();
    const std::shared_ptr<Captured> captured = _captured;
    Core& core = _core;
    ASSERT_TRUE(_core.add_sink(
        callback_sink([&core, captured, flushes_seen_inside](const LogRecord& /*record*/) {
            const std::size_t before = captured->flushes();
            core.flush();
            flushes_seen_inside->push_back(before);
            flushes_seen_inside->push_back(captured->flushes());
        })));
    log_to(_core, "trigger");
    _core.flush();

    ASSERT_EQ(flushes_seen_inside->size(), 2U);
    EXPECT_EQ((*flushes_seen_inside)[0], (*flushes_seen_inside)[1]);
    EXPECT_EQ(_captured->logs().size(), 1U);
}

TEST_F(CoreFlushTest, FlushOnEachOfFourProducerThreadsLeavesNoRecordUndelivered) {
    constexpr int thread_count = 4;
    constexpr int per_thread = 1000;
    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    for (int t = 0; t < thread_count; ++t) {
        threads.emplace_back([this, t] {
            for (int i = 0; i < per_thread; ++i) {
                log_to(_core, "concurrent").tag("t", t);
            }
            _core.flush();
        });
    }
    for (std::thread& t : threads) {
        t.join();
    }
    EXPECT_EQ(_captured->logs().size(), static_cast<std::size_t>(thread_count * per_thread));
}

// Counts the log records of each producer thread, by their "t" tag.
class PerThreadCount {
public:
    explicit PerThreadCount(std::size_t threads) : counts_(threads, 0) {}
    void add(std::size_t thread) {
        const std::scoped_lock lock(mutex_);
        ++counts_.at(thread);
    }
    [[nodiscard]] uint64_t of(std::size_t thread) const {
        const std::scoped_lock lock(mutex_);
        return counts_.at(thread);
    }

private:
    mutable std::mutex mutex_;
    std::vector<uint64_t> counts_;  // guarded by mutex_
};

TEST_F(CoreFlushTest, FlushReturnsOnlyOnceEveryRecordItsThreadSubmittedIsDelivered) {
    // While other threads claim queue slots and publish them, a flush must
    // wait for a slot claimed before it and not yet published, rather than
    // stop at it: the records behind that slot were submitted before the
    // flush too.
    constexpr std::size_t thread_count = 4;
    constexpr uint64_t rounds = 5000;
    auto counts = std::make_shared<PerThreadCount>(thread_count);
    ASSERT_TRUE(_core.add_sink(callback_sink([counts](const LogRecord& r) {
        counts->add(static_cast<std::size_t>(std::stoul(std::string(r.tags.find("t")))));
    })));
    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    for (std::size_t t = 0; t < thread_count; ++t) {
        threads.emplace_back([this, t, counts] {
            for (uint64_t round = 1; round <= rounds; ++round) {
                log_to(_core, "counted").tag("t", t);
                _core.flush();
                if (counts->of(t) != round) {
                    ADD_FAILURE() << "thread " << t << ": " << counts->of(t)
                                  << " records delivered when flush returned, " << round
                                  << " submitted";
                    return;
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
}

TEST(CoreFlushAcrossCoresTest, FlushReachedThroughAnotherCoresCallbackReturnsAtOnce) {
    // Flushes `other` from its own flush(), which only Core::flush() calls,
    // on the thread that called it.
    struct FlushesOtherCore : Sink {
        FlushesOtherCore(Core& core, std::shared_ptr<std::atomic<int>> returned)
            : other(core), flushes_returned(std::move(returned)) {}
        void flush() override {
            other.flush();
            flushes_returned->fetch_add(1);
        }
        Core& other;
        std::shared_ptr<std::atomic<int>> flushes_returned;
    };
    const test::CleanContext context;
    Core first([](std::string_view /*line*/) {});
    Core second([](std::string_view /*line*/) {});
    auto returned = std::make_shared<std::atomic<int>>(0);
    const auto flushes_first = second.add_sink(std::make_unique<FlushesOtherCore>(first, returned));
    ASSERT_TRUE(flushes_first);
    const auto in_second = capture(second);
    ASSERT_TRUE(first.add_sink(callback_sink([&second](const LogRecord& r) {
        if (r.message == "flush the second core") {
            second.flush();  // whose sink then flushes the first, which this thread delivers for
        }
    })));
    log_to(second, "for the second core");
    log_to(first, "flush the second core");
    first.flush();

    EXPECT_EQ(returned->load(), 1);
    EXPECT_EQ(in_second->messages(), (std::vector<std::string>{"for the second core"}));
    // Otherwise the second core's shutdown would flush the first from its
    // sink, the opposite lock order, which ThreadSanitizer reports as a
    // potential deadlock (two threads doing both would deadlock).
    EXPECT_TRUE(second.remove_sink(*flushes_first));
}

TEST(CoreFlushAcrossCoresTest, CallbackThatFlushedAnotherCoreThenFlushesItsOwnReturnsAtOnce) {
    // Leaving the other Core's delivery must leave this thread delivering
    // for its own Core still: its flush() from the callback has to return
    // at once instead of waiting for the delivery it is part of.
    Core other;
    Core core;
    Core* const own = &core;
    Core* const second = &other;
    auto returned = std::make_shared<std::atomic<bool>>(false);
    ASSERT_TRUE(core.add_sink(callback_sink([own, second, returned](const LogRecord& r) {
        if (r.message == "flush both") {
            second->flush();
            own->flush();
            returned->store(true);
        }
    })));
    log_to(core, "flush both");
    core.flush();
    EXPECT_TRUE(returned->load());
}

// Loss accounting.

class CoreStatisticsTest : public LocalCoreTest {};

TEST_F(CoreStatisticsTest, WithABlockedSinkDeliveredPlusDroppedEqualsEmittedExactly) {
    auto gate = std::make_shared<Gate>();
    auto received = std::make_shared<std::atomic<uint64_t>>(0);
    ASSERT_TRUE(_core.add_sink(std::make_unique<GateSink>(gate, received)));

    log_to(_core, "first");
    ASSERT_TRUE(gate->wait_until_entered(1));  // the dispatcher is now stuck in the sink
    constexpr uint64_t extra = 1000;
    const uint64_t emitted = 1 + log_queue_capacity + extra;
    for (uint64_t i = 1; i < emitted; ++i) {
        log_to(_core, "flood");
    }
    const CoreStatistics while_blocked = _core.statistics();
    gate->open();
    _core.flush();

    const CoreStatistics stats = _core.statistics();
    const uint64_t delivered = received->load();
    EXPECT_EQ(delivered + stats.logs.dropped_queue_full, emitted);
    EXPECT_EQ(stats.logs.accepted, delivered);
    EXPECT_EQ(stats.logs.dropped_queue_full, extra);
    EXPECT_EQ(while_blocked.logs.dropped_queue_full, extra);
    EXPECT_EQ(stats.logs.dropped_after_shutdown, 0U);
    EXPECT_EQ(_captured->logs().size(), delivered);
}

TEST_F(CoreStatisticsTest, FirstQueueFullDropIsReportedOnceAsADiagnosticLine) {
    auto gate = std::make_shared<Gate>();
    auto received = std::make_shared<std::atomic<uint64_t>>(0);
    ASSERT_TRUE(_core.add_sink(std::make_unique<GateSink>(gate, received)));
    log_to(_core, "first");
    ASSERT_TRUE(gate->wait_until_entered(1));
    for (uint64_t i = 0; i < log_queue_capacity + 10; ++i) {
        log_to(_core, "flood");
    }
    gate->open();
    _core.flush();
    for (int i = 0; i < 10; ++i) {
        log_to(_core, "after");
    }
    _core.flush();

    const auto lines = _diagnostic_lines();
    ASSERT_EQ(lines.size(), 1U);
    EXPECT_NE(lines[0].find("queue was full"), std::string::npos);
}

TEST_F(CoreStatisticsTest, ASingleRecordRefusedByAFullQueueIsCountedAndReported) {
    auto gate = std::make_shared<Gate>();
    auto received = std::make_shared<std::atomic<uint64_t>>(0);
    ASSERT_TRUE(_core.add_sink(std::make_unique<GateSink>(gate, received)));
    log_to(_core, "first");
    ASSERT_TRUE(gate->wait_until_entered(1));
    // The dispatcher holds "first"; the queue takes exactly its capacity
    // more, and the one record after that is the only one refused.
    for (uint64_t i = 0; i < log_queue_capacity + 1; ++i) {
        log_to(_core, "flood");
    }
    gate->open();
    _core.flush();

    EXPECT_EQ(_core.statistics().logs.dropped_queue_full, 1U);
    EXPECT_EQ(received->load(), log_queue_capacity + 1);
    const auto lines = _diagnostic_lines();
    ASSERT_EQ(lines.size(), 1U);
    EXPECT_NE(lines[0].find("queue was full"), std::string::npos);
}

TEST_F(CoreStatisticsTest, StatisticsCountAcceptedRecordsPerType) {
    for (int i = 0; i < 3; ++i) {
        log_to(_core, "l");
    }
    for (int i = 0; i < 2; ++i) {
        metric_to(_core, "m", 1.0);
    }
    ProgressHandle(_core, "p", 1).finish();
    _core.flush();
    const CoreStatistics stats = _core.statistics();
    EXPECT_EQ(stats.logs.accepted, 3U);
    EXPECT_EQ(stats.metrics.accepted, 2U);
    EXPECT_EQ(stats.progress.accepted, 1U);
    EXPECT_EQ(stats.logs.dropped_queue_full + stats.metrics.dropped_queue_full +
                  stats.progress.dropped_queue_full,
              0U);
    EXPECT_EQ(stats.tags_dropped, 0U);
    EXPECT_TRUE(_diagnostic_lines().empty());
}

TEST_F(CoreStatisticsTest, EmptyDiagnosticHandlerSilencesDiagnostics) {
    _core.set_diagnostic_handler({});
    auto builder_with_too_many_tags = [this] {
        RecordBuilder b(_core, LogLevel::Info, std::source_location::current(), "full");
        for (std::size_t i = 0; i <= max_record_tags; ++i) {
            b.tag("k" + std::to_string(i), i);
        }
    };
    builder_with_too_many_tags();
    _core.flush();
    EXPECT_EQ(_core.statistics().tags_dropped, 1U);
    EXPECT_TRUE(_diagnostic_lines().empty());
}

// The diagnostic handler.

// Records every line it receives and how many calls were running at once;
// the first call blocks on `gate` until the test opens it.
class ObservedHandler {
public:
    void operator()(std::string_view line) {
        const int now = in_flight_.fetch_add(1) + 1;
        int seen = max_in_flight_.load();
        while (now > seen && !max_in_flight_.compare_exchange_weak(seen, now)) {
        }
        if (calls_.fetch_add(1) == 0) {
            gate_.wait_inside();
        }
        {
            const std::scoped_lock lock(mutex_);
            lines_.emplace_back(line);
        }
        changed_.notify_all();
        in_flight_.fetch_sub(1);
    }
    [[nodiscard]] bool wait_for_lines(std::size_t count) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, generous_timeout, [&] { return lines_.size() >= count; });
    }
    [[nodiscard]] std::vector<std::string> lines() {
        const std::scoped_lock lock(mutex_);
        return lines_;
    }
    [[nodiscard]] int max_in_flight() const { return max_in_flight_.load(); }
    [[nodiscard]] Gate& gate() { return gate_; }

private:
    Gate gate_;
    std::atomic<int> in_flight_{0};
    std::atomic<int> max_in_flight_{0};
    std::atomic<int> calls_{0};
    std::mutex mutex_;
    std::condition_variable changed_;  // waits on mutex_
    std::vector<std::string> lines_;   // guarded by mutex_
};

struct ThrowingOnLogSink : Sink {
    void on_log(const LogRecord& /*record*/) override { throw std::runtime_error("disk on fire"); }
    void flush() override {}
};

void log_with_one_tag_too_many(Core& core) {
    RecordBuilder b(core, LogLevel::Info, std::source_location::current(), "full");
    for (std::size_t i = 0; i <= max_record_tags; ++i) {
        b.tag("k" + std::to_string(i), i);
    }
}

TEST(CoreDiagnosticsTest, LineReportedWhileTheHandlerRunsElsewhereIsPassedOnAfterItNotAlongside) {
    const test::CleanContext context;
    auto observed = std::make_shared<ObservedHandler>();
    Core core([observed](std::string_view line) { (*observed)(line); });
    ASSERT_TRUE(core.add_sink(std::make_unique<ThrowingOnLogSink>(), tag_exists("throw")));

    // The dispatcher reports the dropped tag and blocks inside the handler.
    log_with_one_tag_too_many(core);
    ASSERT_TRUE(observed->gate().wait_until_entered(1));
    // This thread delivers the next record and reports the sink's exception
    // while the dispatcher is still inside the handler.
    log_to(core, "throws").tag("throw", true);
    core.flush();
    EXPECT_TRUE(observed->lines().empty());

    observed->gate().open();
    ASSERT_TRUE(observed->wait_for_lines(2));
    const auto lines = observed->lines();
    ASSERT_EQ(lines.size(), 2U);
    EXPECT_NE(lines[0].find("tag was dropped"), std::string::npos);
    EXPECT_NE(lines[1].find("disk on fire"), std::string::npos);
    EXPECT_EQ(observed->max_in_flight(), 1);
}

TEST(CoreDiagnosticsTest, ThrowingHandlerOnEitherThreadLeavesItsLineOnStderrAndDeliveryRunning) {
    struct ThrowingOnFlushSink : Sink {
        void flush() override { throw std::runtime_error("cannot flush"); }
    };
    const test::CleanContext context;
    auto calls = std::make_shared<std::atomic<int>>(0);
    Core core([calls](std::string_view /*line*/) {
        calls->fetch_add(1);
        throw std::runtime_error("handler failed");
    });
    auto captured = capture(core);
    ASSERT_TRUE(core.add_sink(std::make_unique<ThrowingOnFlushSink>()));
    testing::internal::CaptureStderr();

    // On the dispatch thread: the dropped tag, reported without any flush().
    log_with_one_tag_too_many(core);
    const auto deadline = std::chrono::steady_clock::now() + generous_timeout;
    while (calls->load() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    log_to(core, "after the first diagnostic");
    const bool delivery_went_on = captured->wait_for_logs(2, generous_timeout);
    // On this thread: flush() calls the sink's flush, which throws, and
    // reports that here. flush() must return normally.
    core.flush();
    log_to(core, "after the second diagnostic");
    core.flush();

    const std::string err = testing::internal::GetCapturedStderr();
    EXPECT_TRUE(delivery_went_on);
    EXPECT_EQ(calls->load(), 2);
    EXPECT_EQ(captured->logs().size(), 3U);
    EXPECT_NE(err.find("tag was dropped"), std::string::npos);
    EXPECT_NE(err.find("cannot flush"), std::string::npos);
    EXPECT_NE(err.find("the diagnostic handler threw (handler failed)"), std::string::npos);
}

// The sink registry.

class CoreRegistryTest : public LocalCoreTest {};

TEST_F(CoreRegistryTest, AddSinkRejectsANullSinkAndDeliveryContinues) {
    const auto id = _core.add_sink(nullptr);
    ASSERT_FALSE(id.has_value());
    EXPECT_EQ(id.error(), LumenError::NullSink);
    log_to(_core, "still delivered");
    _core.flush();
    EXPECT_EQ(_captured->logs().size(), 1U);
}

TEST_F(CoreRegistryTest, RemovedSinkIsReturnedAndReceivesNothingAfterwards) {
    auto mine = std::make_shared<Captured>();
    const auto id = _core.add_sink(std::make_unique<CaptureSink>(mine));
    ASSERT_TRUE(id);
    log_to(_core, "before");
    _core.flush();
    auto removed = _core.remove_sink(*id);
    ASSERT_TRUE(removed.has_value());
    EXPECT_NE(removed->get(), nullptr);
    log_to(_core, "after");
    _core.flush();
    EXPECT_EQ(mine->messages(), (std::vector<std::string>{"before"}));
    EXPECT_EQ(_captured->messages(), (std::vector<std::string>{"before", "after"}));
}

TEST_F(CoreRegistryTest, RemoveSinkFailsWithInvalidSinkIdForUnknownOrAlreadyRemovedIds) {
    const auto unknown = _core.remove_sink(99999);
    ASSERT_FALSE(unknown.has_value());
    EXPECT_EQ(unknown.error(), LumenError::InvalidSinkId);

    const auto id = _core.add_sink(std::make_unique<NullSink>());
    ASSERT_TRUE(id);
    EXPECT_TRUE(_core.remove_sink(*id).has_value());
    const auto again = _core.remove_sink(*id);
    ASSERT_FALSE(again.has_value());
    EXPECT_EQ(again.error(), LumenError::InvalidSinkId);
}

TEST_F(CoreRegistryTest, RemoveSinkWaitsForItsRunningCallbackAndQueuedRecordsAreNotDelivered) {
    auto gate = std::make_shared<Gate>();
    auto received = std::make_shared<std::atomic<uint64_t>>(0);
    const auto id = _core.add_sink(std::make_unique<GateSink>(gate, received));
    ASSERT_TRUE(id);
    for (int i = 0; i < 5; ++i) {
        log_to(_core, "queued");
    }
    ASSERT_TRUE(gate->wait_until_entered(1));

    std::atomic<uint64_t> received_when_remove_returned{0};
    std::thread remover([&] {
        EXPECT_TRUE(_core.remove_sink(*id).has_value());
        received_when_remove_returned = received->load();
    });
    // remove_sink takes the sink out of the registry before it waits for
    // the callback, so once the id is unknown the remover is past that point.
    while (_core.sink_exception_count(*id).has_value()) {
        std::this_thread::yield();
    }
    EXPECT_EQ(received->load(), 0U);
    gate->open();
    remover.join();
    // The callback that was running completed before remove_sink returned...
    EXPECT_EQ(received_when_remove_returned.load(), 1U);
    _core.flush();
    // ...and the four records still queued never reached the removed sink.
    EXPECT_EQ(received->load(), 1U);
    EXPECT_EQ(_captured->logs().size(), 5U);
}

TEST_F(CoreRegistryTest, SinkCanAddRemoveAndQuerySinksFromInsideItsCallback) {
    const auto other = _core.add_sink(std::make_unique<NullSink>());
    ASSERT_TRUE(other);
    auto results = std::make_shared<std::vector<bool>>();
    Core& core = _core;
    const SinkId other_id = *other;
    ASSERT_TRUE(_core.add_sink(callback_sink([&core, other_id, results](const LogRecord& r) {
        if (r.message != "trigger") return;
        results->push_back(core.add_sink(std::make_unique<NullSink>()).has_value());
        results->push_back(core.sink_exception_count(other_id).has_value());
        results->push_back(core.remove_sink(other_id).has_value());
    })));
    log_to(_core, "trigger");
    _core.flush();
    EXPECT_EQ(*results, (std::vector<bool>{true, true, true}));
}

TEST_F(CoreRegistryTest, SinkRemovedByAnEarlierSinkDuringARecordDoesNotReceiveThatRecord) {
    auto later_id = std::make_shared<std::atomic<SinkId>>(0);
    auto removed = std::make_shared<std::vector<std::unique_ptr<Sink>>>();
    Core& core = _core;
    // Registered before the sink it removes, so it runs first for each record.
    ASSERT_TRUE(_core.add_sink(callback_sink([&core, later_id, removed](const LogRecord& r) {
        if (r.message != "remove now") return;
        auto sink = core.remove_sink(later_id->load());
        if (sink) removed->push_back(std::move(*sink));
    })));
    auto later = std::make_shared<Captured>();
    const auto id = _core.add_sink(std::make_unique<CaptureSink>(later));
    ASSERT_TRUE(id);
    later_id->store(*id);
    log_to(_core, "before");
    log_to(_core, "remove now");
    _core.flush();
    EXPECT_EQ(removed->size(), 1U);
    EXPECT_EQ(later->messages(), (std::vector<std::string>{"before"}));
}

TEST_F(CoreRegistryTest, SinkRemovingItselfFromItsCallbackGetsSinkBusyAndStaysRegistered) {
    auto self_id = std::make_shared<std::atomic<SinkId>>(0);
    auto errors = std::make_shared<std::vector<std::error_code>>();
    auto calls = std::make_shared<std::atomic<int>>(0);
    Core& core = _core;
    const auto id =
        _core.add_sink(callback_sink([&core, self_id, errors, calls](const LogRecord& /*record*/) {
            calls->fetch_add(1);
            const auto result = core.remove_sink(self_id->load());
            if (!result) {
                errors->push_back(result.error());
            }
        }));
    ASSERT_TRUE(id);
    self_id->store(*id);
    log_to(_core, "one");
    log_to(_core, "two");
    _core.flush();
    EXPECT_EQ(calls->load(), 2);  // still registered after the first attempt
    ASSERT_EQ(errors->size(), 2U);
    EXPECT_EQ((*errors)[0], LumenError::SinkBusy);
    EXPECT_TRUE(_core.remove_sink(*id).has_value());
}

// The shape of the Python deadlock: the registering thread holds a lock that
// the running callback needs (the GIL there, a mutex here).
TEST_F(CoreRegistryTest, AddSinkDoesNotWaitForARunningCallback) {
    auto shared_lock = std::make_shared<std::mutex>();
    auto started = std::make_shared<std::atomic<bool>>(false);
    ASSERT_TRUE(_core.add_sink(callback_sink([shared_lock, started](const LogRecord& /*record*/) {
        started->store(true);
        const std::scoped_lock lock(*shared_lock);
    })));
    std::unique_lock held(*shared_lock);
    log_to(_core, "needs the lock");
    // Wait until the callback runs; it then blocks on the lock we hold. If
    // add_sink waited for callbacks, the calls below would never return.
    const auto deadline = std::chrono::steady_clock::now() + generous_timeout;
    while (!started->load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    ASSERT_TRUE(started->load());
    const auto added = _core.add_sink(std::make_unique<NullSink>());
    ASSERT_TRUE(added.has_value());
    EXPECT_TRUE(_core.sink_exception_count(*added).has_value());
    EXPECT_TRUE(_core.remove_sink(*added).has_value());
    held.unlock();
    _core.flush();
}

TEST_F(CoreRegistryTest, ExceptionsFromASinkAreCountedReportedOnceAndOtherSinksStillReceive) {
    struct ThrowingSink : Sink {
        void on_log(const LogRecord& /*record*/) override {
            throw std::runtime_error("disk on fire");
        }
        void flush() override { throw 42; }  // not a std::exception
    };
    const auto bad = _core.add_sink(std::make_unique<ThrowingSink>());
    ASSERT_TRUE(bad);
    log_to(_core, "first");
    log_to(_core, "second");
    _core.flush();

    EXPECT_EQ(_captured->logs().size(), 2U);
    const auto count = _core.sink_exception_count(*bad);
    ASSERT_TRUE(count.has_value());
    EXPECT_EQ(*count, 3U);  // two on_log, one flush
    const auto lines = _diagnostic_lines();
    ASSERT_EQ(lines.size(), 1U);
    EXPECT_NE(lines[0].find("disk on fire"), std::string::npos);
}

TEST_F(CoreRegistryTest, SinkExceptionCountForAnUnknownIdFailsWithInvalidSinkId) {
    const auto count = _core.sink_exception_count(SinkId{0});
    ASSERT_FALSE(count.has_value());
    EXPECT_EQ(count.error(), LumenError::InvalidSinkId);
}

// Shutdown.

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
        for (int i = 0; i < 1000; ++i) {
            log_to(core, "queued at destruction");
        }
    }
    EXPECT_EQ(captured->logs().size(), 1000U);
    EXPECT_EQ(captured->flushes(), 1U);
    EXPECT_TRUE(destroyed->load());
}

// Calls back into its Core from its destructor and records what each call
// returned.
class CallsCoreWhenDestroyed : public Sink {
public:
    struct Results {
        std::error_code add_sink;
        std::error_code remove_sink;
        std::error_code sink_exception_count;
        bool destroyed = false;
    };
    CallsCoreWhenDestroyed(Core& core, SinkId other, std::shared_ptr<Results> results)
        : core_(core), other_(other), results_(std::move(results)) {}
    ~CallsCoreWhenDestroyed() override {
        auto added = core_.add_sink(std::make_unique<NullSink>());
        results_->add_sink = added ? std::error_code{} : added.error();
        auto removed = core_.remove_sink(other_);
        results_->remove_sink = removed ? std::error_code{} : removed.error();
        auto count = core_.sink_exception_count(other_);
        results_->sink_exception_count = count ? std::error_code{} : count.error();
        results_->destroyed = true;
    }
    CallsCoreWhenDestroyed(const CallsCoreWhenDestroyed&) = delete;
    CallsCoreWhenDestroyed& operator=(const CallsCoreWhenDestroyed&) = delete;
    CallsCoreWhenDestroyed(CallsCoreWhenDestroyed&&) = delete;
    CallsCoreWhenDestroyed& operator=(CallsCoreWhenDestroyed&&) = delete;
    void flush() override {}

private:
    Core& core_;
    SinkId other_;
    std::shared_ptr<Results> results_;
};

TEST(CoreShutdownTest, SinkDestructorCallingItsCoreAtShutdownGetsErrorsInsteadOfADeadlock) {
    const test::CleanContext context;
    auto results = std::make_shared<CallsCoreWhenDestroyed::Results>();
    {
        Core core([](std::string_view /*line*/) {});
        const auto other = core.add_sink(std::make_unique<NullSink>());
        ASSERT_TRUE(other);
        ASSERT_TRUE(core.add_sink(std::make_unique<CallsCoreWhenDestroyed>(core, *other, results)));
    }
    ASSERT_TRUE(results->destroyed);
    EXPECT_EQ(results->add_sink, LumenError::CoreShutDown);
    EXPECT_EQ(results->remove_sink, LumenError::InvalidSinkId);
    EXPECT_EQ(results->sink_exception_count, LumenError::InvalidSinkId);
}

// Fatal records.

TEST(CoreFatalDeathTest,
     FatalRecordIsDeliveredAfterEarlierRecordsAndSinksAreFlushedBeforeTermination) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            Core core;
            EXPECT_TRUE(core.add_sink(std::make_unique<StderrSink>()));
            log_to(core, "earlier");
            log_to(core, "boom", LogLevel::Fatal);
        },
        "delivered:earlier\ndelivered:boom\nflushed");
}

TEST(CoreFatalDeathTest,
     FatalFromASinkCallbackGivesTheOtherSinksTheRecordInFlightThenTheQueueThenTheFatal) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            Core core;
            Core* const target = &core;
            EXPECT_TRUE(core.add_sink(callback_sink([target](const LogRecord& r) {
                if (r.message == "trigger") {
                    // Raise it only once the next record is queued too.
                    while (target->statistics().logs.accepted < 2) {
                        std::this_thread::yield();
                    }
                    log_to(*target, "fatal from callback", LogLevel::Fatal);
                }
            })));
            EXPECT_TRUE(core.add_sink(std::make_unique<StderrSink>()));
            log_to(core, "trigger");
            log_to(core, "queued after the trigger");
            core.flush();
        },
        // The sink that raised the fatal record is skipped; the other one
        // comes later in the list and still receives "trigger".
        "^delivered:trigger\ndelivered:queued after the trigger\ndelivered:fatal from "
        "callback\nflushed");
}

TEST(CoreFatalDeathTest, FatalRaisedOnASecondThreadWhileTheFirstIsDeliveredIsDeliveredToo) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            Core core;
            Core* const target = &core;
            std::thread second;  // ends with the process, never joined
            EXPECT_TRUE(core.add_sink(callback_sink([target, &second](const LogRecord& r) {
                if (r.message != "fatal from the first thread") return;
                const uint64_t before = target->statistics().logs.accepted;
                second = std::thread(
                    [target] { log_to(*target, "fatal from the second thread", LogLevel::Fatal); });
                // Return only once the second record has been submitted.
                while (target->statistics().logs.accepted == before) {
                    std::this_thread::yield();
                }
            })));
            EXPECT_TRUE(core.add_sink(std::make_unique<StderrSink>()));
            log_to(core, "fatal from the first thread", LogLevel::Fatal);
        },
        "delivered:fatal from the first thread\ndelivered:fatal from the second thread\nflushed");
}

TEST(CoreFatalDeathTest, SinkReceivingAFatalRaisedByAnotherSinkCannotRemoveItAndGetsSinkBusy) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            Core core;
            Core* const target = &core;
            const auto raiser = core.add_sink(callback_sink([target](const LogRecord& r) {
                if (r.message == "trigger") {
                    log_to(*target, "fatal from callback", LogLevel::Fatal);
                }
            }));
            EXPECT_TRUE(raiser);
            const SinkId raiser_id = raiser.value_or(0);
            EXPECT_TRUE(core.add_sink(callback_sink([target, raiser_id](const LogRecord& r) {
                if (r.message == "fatal from callback") {
                    const auto removed = target->remove_sink(raiser_id);
                    std::cerr << "remove_sink: "
                              << (removed ? std::string("removed") : removed.error().message())
                              << '\n';
                }
            })));
            log_to(core, "trigger");
            core.flush();
        },
        "remove_sink: sink is busy in a callback on this thread");
}

// Bounded flush.

// Records on which thread its flush() runs.
class FlushThreadSink : public Sink {
public:
    explicit FlushThreadSink(std::shared_ptr<std::atomic<std::thread::id>> flushed_on)
        : flushed_on_(std::move(flushed_on)) {}
    void flush() override { flushed_on_->store(std::this_thread::get_id()); }

private:
    std::shared_ptr<std::atomic<std::thread::id>> flushed_on_;
};

TEST_F(CoreFlushTest, FlushForDeliversEverythingAndFlushesTheSinksOnTheDispatchThread) {
    auto flushed_on = std::make_shared<std::atomic<std::thread::id>>();
    ASSERT_TRUE(_core.add_sink(std::make_unique<FlushThreadSink>(flushed_on)));
    for (int i = 0; i < 500; ++i) {
        log_to(_core, "record");
    }
    const std::size_t flushes_before = _captured->flushes();

    const std::expected<void, std::error_code> flushed = _core.flush_for(generous_timeout);

    EXPECT_TRUE(flushed.has_value());
    EXPECT_EQ(_captured->logs().size(), 500U);
    EXPECT_EQ(_captured->flushes(), flushes_before + 1);
    EXPECT_NE(flushed_on->load(), std::thread::id{});
    EXPECT_NE(flushed_on->load(), std::this_thread::get_id());
}

TEST_F(CoreFlushTest, FlushForBehindABlockedCallbackTimesOutAndTheFlushIsMadeOnceItReturns) {
    auto gate = std::make_shared<Gate>();
    auto received = std::make_shared<std::atomic<uint64_t>>(0);
    ASSERT_TRUE(_core.add_sink(std::make_unique<GateSink>(gate, received)));
    log_to(_core, "blocks");
    ASSERT_TRUE(gate->wait_until_entered(1));
    const std::size_t flushes_before = _captured->flushes();

    const std::expected<void, std::error_code> flushed =
        _core.flush_for(std::chrono::milliseconds{50});

    ASSERT_FALSE(flushed.has_value());
    EXPECT_EQ(flushed.error(), LumenError::FlushTimedOut);
    EXPECT_EQ(_captured->flushes(), flushes_before);
    gate->open();
    _core.flush();
    // The request that timed out was served too, before or with this flush.
    EXPECT_GE(_captured->flushes(), flushes_before + 1);
    EXPECT_EQ(received->load(), 1U);
}

TEST_F(CoreFlushTest, FlushForFromInsideACallbackOfTheSameCoreFailsWithSinkBusy) {
    auto result = std::make_shared<std::error_code>();
    Core& core = _core;
    ASSERT_TRUE(_core.add_sink(callback_sink([&core, result](const LogRecord& /*record*/) {
        const std::expected<void, std::error_code> flushed = core.flush_for(generous_timeout);
        *result = flushed ? std::error_code{} : flushed.error();
    })));
    log_to(_core, "trigger");
    _core.flush();

    EXPECT_EQ(*result, LumenError::SinkBusy);
}

// Flushes its Core from its destructor, which runs at that Core's shutdown.
class FlushesWhenDestroyed : public Sink {
public:
    FlushesWhenDestroyed(Core& core, std::shared_ptr<std::error_code> result)
        : core_(core), result_(std::move(result)) {}
    ~FlushesWhenDestroyed() override {
        const std::expected<void, std::error_code> flushed = core_.flush_for(generous_timeout);
        *result_ = flushed ? std::error_code{} : flushed.error();
    }
    FlushesWhenDestroyed(const FlushesWhenDestroyed&) = delete;
    FlushesWhenDestroyed& operator=(const FlushesWhenDestroyed&) = delete;
    FlushesWhenDestroyed(FlushesWhenDestroyed&&) = delete;
    FlushesWhenDestroyed& operator=(FlushesWhenDestroyed&&) = delete;
    void flush() override {}

private:
    Core& core_;
    std::shared_ptr<std::error_code> result_;
};

TEST(CoreShutdownTest, FlushForAfterShutdownFailsWithCoreShutDown) {
    const test::CleanContext context;
    auto result = std::make_shared<std::error_code>();
    {
        Core core([](std::string_view /*line*/) {});
        ASSERT_TRUE(core.add_sink(std::make_unique<FlushesWhenDestroyed>(core, result)));
    }
    EXPECT_EQ(*result, LumenError::CoreShutDown);
}

// Two Cores whose dispatch threads, each inside a callback, flush the other
// Core: each callback waits until both have started, then flushes the other
// Core with the given function and records what it returned.
class CrossFlush {
public:
    using Flush = std::error_code (*)(Core& other);

    explicit CrossFlush(Flush flush) {
        add_flusher_(first_, second_, 0, flush);
        add_flusher_(second_, first_, 1, flush);
    }

    // Starts both and waits for both flushes to return; false if they did not.
    [[nodiscard]] bool run() {
        log_to(first_, "cross");
        log_to(second_, "cross");
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, generous_timeout, [this] { return returned_ == 2; });
    }

    [[nodiscard]] std::array<std::error_code, 2> results() {
        const std::scoped_lock lock(mutex_);
        return results_;
    }

private:
    void add_flusher_(Core& own, Core& other, std::size_t slot, Flush flush) {
        EXPECT_TRUE(own.add_sink(callback_sink([this, &other, slot, flush](const LogRecord& r) {
            if (r.message != "cross") {
                return;
            }
            arrive_and_wait_();
            const std::error_code result = flush(other);
            const std::scoped_lock lock(mutex_);
            results_.at(slot) = result;
            ++returned_;
            changed_.notify_all();
        })));
    }

    void arrive_and_wait_() {
        std::unique_lock lock(mutex_);
        ++arrived_;
        changed_.notify_all();
        changed_.wait(lock, [this] { return arrived_ == 2; });
    }

    const test::CleanContext context_;
    std::mutex mutex_;
    std::condition_variable changed_;           // waits on mutex_
    uint32_t arrived_ = 0;                      // guarded by mutex_
    uint32_t returned_ = 0;                     // guarded by mutex_
    std::array<std::error_code, 2> results_{};  // guarded by mutex_
    // Last: destroyed first, while the members their callbacks use exist.
    Core first_{[](std::string_view /*line*/) {}};
    Core second_{[](std::string_view /*line*/) {}};
};

TEST(CrossFlushTest, FlushForBetweenTwoDispatchThreadsReturnsSinkBusyInsteadOfDeadlocking) {
    CrossFlush cross([](Core& other) {
        const std::expected<void, std::error_code> flushed = other.flush_for(generous_timeout);
        return flushed ? std::error_code{} : flushed.error();
    });

    ASSERT_TRUE(cross.run());
    const std::array<std::error_code, 2> results = cross.results();
    // At least one sees the cycle; the other then either sees it too or is
    // served once the first returns from its callback.
    EXPECT_TRUE(results[0] == LumenError::SinkBusy || results[1] == LumenError::SinkBusy);
    for (const std::error_code& result : results) {
        EXPECT_TRUE(!result || result == LumenError::SinkBusy) << result.message();
    }
}

TEST(CrossFlushTest, FlushBetweenTwoDispatchThreadsReturnsInsteadOfDeadlocking) {
    CrossFlush cross([](Core& other) {
        other.flush();
        return std::error_code{};
    });

    EXPECT_TRUE(cross.run());
}

TEST_F(CoreStatisticsTest, QueuedCountsRecordsAcceptedAndNotYetHandedToTheSinks) {
    auto gate = std::make_shared<Gate>();
    auto received = std::make_shared<std::atomic<uint64_t>>(0);
    ASSERT_TRUE(_core.add_sink(std::make_unique<GateSink>(gate, received)));
    log_to(_core, "blocks");
    ASSERT_TRUE(gate->wait_until_entered(1));
    for (int i = 0; i < 5; ++i) {
        log_to(_core, "queued");
    }
    metric_to(_core, "queued", 1.0);

    const CoreStatistics blocked = _core.statistics();
    gate->open();
    _core.flush();
    const CoreStatistics delivered = _core.statistics();

    EXPECT_EQ(blocked.logs.queued, 5U);
    EXPECT_EQ(blocked.metrics.queued, 1U);
    EXPECT_EQ(delivered.logs.queued, 0U);
    EXPECT_EQ(delivered.metrics.queued, 0U);
}

// Bounded shutdown.

// Lines a Core reports, from any thread.
struct ReportedLines {
    std::mutex mutex;
    std::vector<std::string> lines;  // guarded by mutex

    [[nodiscard]] std::vector<std::string> copy() {
        const std::scoped_lock lock(mutex);
        return lines;
    }
};

[[nodiscard]] std::unique_ptr<Core> core_reporting_to(const std::shared_ptr<ReportedLines>& to) {
    return std::make_unique<Core>([to](std::string_view line) {
        const std::scoped_lock lock(to->mutex);
        to->lines.emplace_back(line);
    });
}

TEST(CoreShutdownTest, ShutdownGivesUpOnACallbackThatDoesNotReturnAndReportsTheQueuedCount) {
    const test::CleanContext context;
    auto reported = std::make_shared<ReportedLines>();
    auto gate = std::make_shared<Gate>();
    auto received = std::make_shared<std::atomic<uint64_t>>(0);
    std::unique_ptr<Core> core = core_reporting_to(reported);
    ASSERT_TRUE(core->add_sink(std::make_unique<GateSink>(gate, received)));
    core->set_shutdown_timeout(std::chrono::milliseconds{100});
    log_to(*core, "blocks");
    ASSERT_TRUE(gate->wait_until_entered(1));
    for (int i = 0; i < 3; ++i) {
        log_to(*core, "queued behind it");
    }

    const auto start = std::chrono::steady_clock::now();
    core.reset();
    const auto took = std::chrono::steady_clock::now() - start;
    const uint64_t received_by_then = received->load();
    // The abandoned dispatch thread delivers the rest and ends once released;
    // the abandoned Core stays allocated for it.
    gate->open();

    EXPECT_GE(took, std::chrono::milliseconds{100});
    EXPECT_LT(took, generous_timeout);
    EXPECT_EQ(received_by_then, 0U);
    const std::vector<std::string> lines = reported->copy();
    ASSERT_EQ(lines.size(), 1U);
    EXPECT_NE(lines.front().find("shutdown gave up after 100 ms"), std::string::npos);
    EXPECT_NE(lines.front().find("3 queued record(s) were not delivered"), std::string::npos);
}

// Blocks on the record "hold" until its gate opens; takes 20 ms over every
// other record, and says when the first of those arrives.
class SlowSink : public Sink {
public:
    SlowSink(std::shared_ptr<Gate> hold, std::shared_ptr<Gate> first_slow,
             std::shared_ptr<std::atomic<uint64_t>> received)
        : hold_(std::move(hold))
        , first_slow_(std::move(first_slow))
        , received_(std::move(received)) {}
    void on_log(const LogRecord& r) override {
        if (r.message == "hold") {
            hold_->wait_inside();
            return;
        }
        if (received_->load() == 0) {
            first_slow_->open();
            first_slow_->wait_inside();  // open already: marks it entered
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        received_->fetch_add(1);
    }
    void flush() override {}

private:
    std::shared_ptr<Gate> hold_;
    std::shared_ptr<Gate> first_slow_;
    std::shared_ptr<std::atomic<uint64_t>> received_;
};

// The dispatch thread is in one pass over 25 slow records (500 ms) when the
// Core is destroyed with a shutdown timeout of 250 ms: delivery keeps making
// progress, so shutdown waits for all of it.
TEST(CoreShutdownTest, ShutdownWaitsLongerThanItsTimeoutForASinkThatKeepsMakingProgress) {
    const test::CleanContext context;
    auto reported = std::make_shared<ReportedLines>();
    auto hold = std::make_shared<Gate>();
    auto first_slow = std::make_shared<Gate>();
    auto received = std::make_shared<std::atomic<uint64_t>>(0);
    {
        std::unique_ptr<Core> core = core_reporting_to(reported);
        ASSERT_TRUE(core->add_sink(std::make_unique<SlowSink>(hold, first_slow, received)));
        core->set_shutdown_timeout(std::chrono::milliseconds{250});
        log_to(*core, "hold");
        ASSERT_TRUE(hold->wait_until_entered(1));
        for (int i = 0; i < 25; ++i) {
            log_to(*core, "slow");
        }
        // The next pass reads the queue heads after all 25 were queued.
        hold->open();
        ASSERT_TRUE(first_slow->wait_until_entered(1));
    }
    EXPECT_EQ(received->load(), 25U);
    EXPECT_TRUE(reported->copy().empty());
}

// How a process that aborted itself ended: POSIX reports the signal, Windows
// an exit code of 3.
[[nodiscard]] auto aborted() {
#if defined(PLATFORM_WINDOWS)
    return testing::ExitedWithCode(3);
#else
    return testing::KilledBySignal(SIGABRT);
#endif
}

TEST(CoreFatalDeathTest, FatalWithTheDefaultTerminateHandlerAbortsWithoutTheHandlersMessage) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            Core core;
            EXPECT_TRUE(core.add_sink(std::make_unique<StderrSink>()));
            log_to(core, "boom", LogLevel::Fatal);
        },
        aborted(), "^delivered:boom\nflushed\n$");
}

TEST(CoreFatalDeathTest, FatalCallsTheProgramsOwnTerminateHandler) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            std::set_terminate([] {
                std::cerr << "the program's handler\n" << std::flush;
                std::abort();
            });
            Core core;
            EXPECT_TRUE(core.add_sink(std::make_unique<StderrSink>()));
            log_to(core, "boom", LogLevel::Fatal);
        },
        aborted(), "flushed\nthe program's handler\n$");
}

TEST(CoreFatalDeathTest, FatalWhileACallbackNeverReturnsEndsTheProcessAfterTheShutdownTimeout) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            Core core;
            auto gate = std::make_shared<Gate>();
            auto received = std::make_shared<std::atomic<uint64_t>>(0);
            EXPECT_TRUE(core.add_sink(std::make_unique<GateSink>(gate, received)));
            core.set_shutdown_timeout(std::chrono::milliseconds{50});
            log_to(core, "blocks");
            EXPECT_TRUE(gate->wait_until_entered(1));
            log_to(core, "boom", LogLevel::Fatal);
        },
        aborted(),
        "lumen: a fatal record was not delivered: the thread delivering records made no "
        "progress for 50 ms");
}

}  // namespace
}  // namespace lumen
