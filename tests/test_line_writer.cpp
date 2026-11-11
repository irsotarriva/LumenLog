// LineWriter (src/line_writer.cpp), the writer thread of the line sinks:
// order, the queue bound, failure accounting, where diagnostics are called,
// and the bounded flush after a fatal record.
#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <future>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "line_writer.hpp"
#include "lumen/writer_statistics.hpp"

namespace lumen::detail {
namespace {

// A gate that callers wait at until the test opens it.
class Gate {
public:
    void open() {
        {
            const std::scoped_lock lock(mutex_);
            open_ = true;
        }
        changed_.notify_all();
    }
    void pass() {
        std::unique_lock lock(mutex_);
        arrived_ = true;
        changed_.notify_all();
        changed_.wait(lock, [&] { return open_; });
    }
    void wait_until_arrived() {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [&] { return arrived_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;  // waits on mutex_
    bool open_ = false;                // guarded by mutex_
    bool arrived_ = false;             // guarded by mutex_
};

// What the writer was given, and what its diagnostic handler received.
struct Recorded {
    std::mutex mutex;
    std::vector<std::string> lines;                   // guarded by mutex
    std::vector<std::string> diagnostics;             // guarded by mutex
    std::vector<std::thread::id> diagnostic_threads;  // guarded by mutex

    [[nodiscard]] std::vector<std::string> all_lines() {
        const std::scoped_lock lock(mutex);
        return lines;
    }
    [[nodiscard]] std::vector<std::string> all_diagnostics() {
        const std::scoped_lock lock(mutex);
        return diagnostics;
    }
};

[[nodiscard]] DiagnosticHandler recording_handler(const std::shared_ptr<Recorded>& recorded) {
    return [recorded](std::string_view line) {
        const std::scoped_lock lock(recorded->mutex);
        recorded->diagnostics.emplace_back(line);
        recorded->diagnostic_threads.push_back(std::this_thread::get_id());
    };
}

[[nodiscard]] BatchWriter recording_writer(const std::shared_ptr<Recorded>& recorded) {
    return [recorded](std::span<const std::string> lines) {
        const std::scoped_lock lock(recorded->mutex);
        recorded->lines.insert(recorded->lines.end(), lines.begin(), lines.end());
        return BatchOutcome{};
    };
}

[[nodiscard]] std::unique_ptr<LineWriter> make_writer(std::size_t max_queued_lines,
                                                      BatchWriter write,
                                                      DiagnosticHandler diagnostics) {
    std::expected<std::unique_ptr<LineWriter>, std::error_code> writer =
        LineWriter::create("TestSink", max_queued_lines, std::move(write), std::move(diagnostics));
    EXPECT_TRUE(writer.has_value());
    return writer ? std::move(*writer) : nullptr;
}

TEST(LineWriterTest, LinesReachTheDestinationInOrderWhenWaitUntilWrittenReturns) {
    const auto recorded = std::make_shared<Recorded>();
    const auto writer = make_writer(100, recording_writer(recorded), recording_handler(recorded));
    ASSERT_NE(writer, nullptr);
    for (uint32_t i = 0; i < 50; ++i) {
        writer->enqueue(std::to_string(i));
    }
    writer->wait_until_written();
    const std::vector<std::string> lines = recorded->all_lines();
    ASSERT_EQ(lines.size(), 50U);
    for (uint32_t i = 0; i < 50; ++i) {
        EXPECT_EQ(lines[i], std::to_string(i));
    }
    const WriterStatistics stats = writer->statistics();
    EXPECT_EQ(stats.lines_queued, 50U);
    EXPECT_EQ(stats.lines_written, 50U);
    EXPECT_EQ(stats.lines_dropped, 0U);
    EXPECT_EQ(stats.lines_failed, 0U);
    EXPECT_TRUE(recorded->all_diagnostics().empty());
}

TEST(LineWriterTest, DestructionWritesTheLinesStillQueued) {
    const auto recorded = std::make_shared<Recorded>();
    {
        const auto writer = make_writer(100, recording_writer(recorded), {});
        ASSERT_NE(writer, nullptr);
        writer->enqueue("one");
        writer->enqueue("two");
    }
    EXPECT_EQ(recorded->all_lines(), (std::vector<std::string>{"one", "two"}));
}

// The destination blocks on its first batch and the diagnostic handler blocks
// too. Enqueueing must still return at once: the drop is counted, and it is
// reported later by the writer thread, never by the enqueuing thread.
TEST(LineWriterTest, DropIsCountedAtOnceAndReportedOnceByTheWriterThreadNotByTheCaller) {
    const auto recorded = std::make_shared<Recorded>();
    const auto destination = std::make_shared<Gate>();
    const auto handler_gate = std::make_shared<Gate>();
    const BatchWriter write = [recorded, destination](std::span<const std::string> lines) {
        destination->pass();
        return recording_writer(recorded)(lines);
    };
    const DiagnosticHandler diagnostics = [recorded, handler_gate](std::string_view line) {
        handler_gate->pass();
        recording_handler(recorded)(line);
    };
    const auto writer = make_writer(1, write, diagnostics);
    ASSERT_NE(writer, nullptr);
    writer->enqueue("1");
    destination->wait_until_arrived();  // "1" is held in the destination

    std::future<std::thread::id> caller = std::async(std::launch::async, [&] {
        writer->enqueue("2");  // queued
        writer->enqueue("3");  // dropped
        writer->enqueue("4");  // dropped
        return std::this_thread::get_id();
    });
    const bool returned = caller.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
    EXPECT_TRUE(returned) << "enqueue waited for the destination or the diagnostic handler";
    EXPECT_EQ(writer->statistics().lines_dropped, 2U);
    destination->open();
    handler_gate->open();
    if (!returned) {
        caller.wait();
    }
    writer->wait_until_written();

    EXPECT_EQ(recorded->all_lines(), (std::vector<std::string>{"1", "2"}));
    const std::scoped_lock lock(recorded->mutex);
    ASSERT_EQ(recorded->diagnostics.size(), 1U);
    EXPECT_EQ(recorded->diagnostics.front(),
              "lumen: TestSink: output queue full (1 lines); lines are dropped while it stays "
              "full and only counted (statistics().lines_dropped)");
    ASSERT_EQ(recorded->diagnostic_threads.size(), 1U);
    EXPECT_NE(recorded->diagnostic_threads.front(), caller.get());
}

TEST(LineWriterTest, FailedLinesAreCountedAndOnlyTheFirstFailureIsReported) {
    const auto recorded = std::make_shared<Recorded>();
    const BatchWriter failing = [](std::span<const std::string> lines) {
        return BatchOutcome{.failed_lines = lines.size(), .failure = "disk on fire"};
    };
    const auto writer = make_writer(100, failing, recording_handler(recorded));
    ASSERT_NE(writer, nullptr);
    for (uint32_t i = 0; i < 3; ++i) {
        writer->enqueue("x");
        writer->wait_until_written();
    }
    const WriterStatistics stats = writer->statistics();
    EXPECT_EQ(stats.lines_queued, 3U);
    EXPECT_EQ(stats.lines_written, 0U);
    EXPECT_EQ(stats.lines_failed, 3U);
    EXPECT_EQ(recorded->all_diagnostics(),
              (std::vector<std::string>{"lumen: TestSink: write failed (disk on fire); later "
                                        "failures are only counted (statistics().lines_failed)"}));
}

TEST(LineWriterTest, FailureWithoutLostLinesIsReportedAndCountsNothingAsFailed) {
    const auto recorded = std::make_shared<Recorded>();
    const BatchWriter rotation_failed = [](std::span<const std::string> /*lines*/) {
        return BatchOutcome{.failed_lines = 0, .failure = "cannot rename"};
    };
    const auto writer = make_writer(100, rotation_failed, recording_handler(recorded));
    ASSERT_NE(writer, nullptr);
    writer->enqueue("x");
    writer->wait_until_written();
    EXPECT_EQ(writer->statistics().lines_written, 1U);
    EXPECT_EQ(writer->statistics().lines_failed, 0U);
    const std::vector<std::string> reported = recorded->all_diagnostics();
    ASSERT_EQ(reported.size(), 1U);
    EXPECT_NE(reported.front().find("write failed (cannot rename)"), std::string::npos);
}

TEST(LineWriterTest, FailureWithoutAReasonIsReportedAsNoDetailAndCountsAtMostTheBatch) {
    // The destination claims more failed lines than it was given and says
    // nothing about why: the count is capped at the one line of the batch.
    const auto recorded = std::make_shared<Recorded>();
    const BatchWriter silent_failure = [](std::span<const std::string> /*lines*/) {
        return BatchOutcome{.failed_lines = 5, .failure = ""};
    };
    const auto writer = make_writer(100, silent_failure, recording_handler(recorded));
    ASSERT_NE(writer, nullptr);
    writer->enqueue("x");
    writer->wait_until_written();
    EXPECT_EQ(writer->statistics().lines_written, 0U);
    EXPECT_EQ(writer->statistics().lines_failed, 1U);
    EXPECT_EQ(recorded->all_diagnostics(),
              (std::vector<std::string>{"lumen: TestSink: write failed (no detail); later "
                                        "failures are only counted (statistics().lines_failed)"}));
}

TEST(LineWriterTest, ExceptionFromTheDestinationFailsTheWholeBatchWithItsMessage) {
    const auto recorded = std::make_shared<Recorded>();
    const BatchWriter throwing = [](std::span<const std::string> /*lines*/) -> BatchOutcome {
        throw std::runtime_error("stream exploded");
    };
    const auto writer = make_writer(100, throwing, recording_handler(recorded));
    ASSERT_NE(writer, nullptr);
    writer->enqueue("a");
    writer->wait_until_written();
    EXPECT_EQ(writer->statistics().lines_failed, 1U);
    const std::vector<std::string> reported = recorded->all_diagnostics();
    ASSERT_EQ(reported.size(), 1U);
    EXPECT_NE(reported.front().find("write failed (stream exploded)"), std::string::npos);
}

TEST(LineWriterTest, ThrowingDiagnosticHandlerIsContainedAndWritingGoesOn) {
    const auto recorded = std::make_shared<Recorded>();
    const DiagnosticHandler throwing = [](std::string_view /*line*/) {
        throw std::runtime_error("handler failed");
    };
    const auto writer = make_writer(1, recording_writer(recorded), throwing);
    ASSERT_NE(writer, nullptr);
    for (uint32_t i = 0; i < 20; ++i) {
        writer->enqueue("x");
    }
    writer->wait_until_written();
    writer->enqueue("after");
    writer->wait_until_written();
    const std::vector<std::string> lines = recorded->all_lines();
    ASSERT_FALSE(lines.empty());
    EXPECT_EQ(lines.back(), "after");
    const WriterStatistics stats = writer->statistics();
    EXPECT_EQ(stats.lines_queued + stats.lines_dropped, 21U);
    EXPECT_EQ(stats.lines_queued, stats.lines_written);
}

TEST(LineWriterTest, AfterExpectTerminationWaitUntilWrittenGivesUpAfterTheFatalTimeout) {
    const auto destination = std::make_shared<Gate>();
    const BatchWriter blocked = [destination](std::span<const std::string> /*lines*/) {
        destination->pass();
        return BatchOutcome{};
    };
    const auto writer = make_writer(10, blocked, {});
    ASSERT_NE(writer, nullptr);
    writer->enqueue("fatal");
    destination->wait_until_arrived();
    writer->expect_termination();
    const auto start = std::chrono::steady_clock::now();
    writer->wait_until_written();
    const auto waited = std::chrono::steady_clock::now() - start;
    EXPECT_GE(waited, fatal_flush_timeout);
    EXPECT_LT(waited, fatal_flush_timeout + std::chrono::seconds(5));
    EXPECT_EQ(writer->statistics().lines_written, 0U);  // still held by the destination
    destination->open();
}

}  // namespace
}  // namespace lumen::detail
