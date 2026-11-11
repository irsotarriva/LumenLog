// A program built the way a user builds one, against an installed or a
// source-tree Lumen. It checks that the settings Lumen was built with
// (cmake/build_settings.cmake) are the ones it sees and the ones the library
// has:
//   - the level threshold: each LUMEN_LOG_* below it delivers nothing, each
//     one at or above it delivers its record;
//   - the queue capacities: lumen::*_queue_capacity, and the number of
//     records each queue of the library accepts while the dispatcher is held
//     in a sink.
// The expected values come from the build of this program as
// CONSUMER_EXPECT_*. Exit status 0 when everything matches; otherwise 1,
// with one line per mismatch on standard error.
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <source_location>
#include <string>
#include <utility>
#include <vector>

#include "lumen/lumen.hpp"

#if !defined(CONSUMER_EXPECT_MIN_LEVEL) || !defined(CONSUMER_EXPECT_LOG_CAPACITY) || \
    !defined(CONSUMER_EXPECT_METRIC_CAPACITY) || !defined(CONSUMER_EXPECT_PROGRESS_CAPACITY)
#error "build this file with the CONSUMER_EXPECT_* values (tests/consumer/CMakeLists.txt)"
#endif

namespace {

inline constexpr int32_t expected_min_level = CONSUMER_EXPECT_MIN_LEVEL;
inline constexpr uint64_t expected_log_capacity = CONSUMER_EXPECT_LOG_CAPACITY;
inline constexpr uint64_t expected_metric_capacity = CONSUMER_EXPECT_METRIC_CAPACITY;
inline constexpr uint64_t expected_progress_capacity = CONSUMER_EXPECT_PROGRESS_CAPACITY;
// Records submitted beyond a full queue, each of which must be refused.
inline constexpr uint64_t overflow = 10;

// Copies each log message: a record's views are only valid during the callback.
class RecordingSink : public lumen::Sink {
public:
    explicit RecordingSink(std::shared_ptr<std::vector<std::string>> messages)
        : messages_(std::move(messages)) {}

    void on_log(const lumen::LogRecord& record) override {
        messages_->emplace_back(record.message);
    }
    void flush() override {}

private:
    std::shared_ptr<std::vector<std::string>> messages_;
};

// Holds the dispatcher inside the first on_log until open() is called, so
// that nothing leaves the queues meanwhile.
class Gate {
public:
    void enter_and_wait() {
        std::unique_lock lock(mutex_);
        entered_ = true;
        changed_.notify_all();
        changed_.wait(lock, [this] { return open_; });
    }
    void wait_until_entered() {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [this] { return entered_; });
    }
    void open() {
        const std::scoped_lock lock(mutex_);
        open_ = true;
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool entered_ = false;  // guarded by mutex_
    bool open_ = false;     // guarded by mutex_
};

class GateSink : public lumen::Sink {
public:
    explicit GateSink(std::shared_ptr<Gate> gate) : gate_(std::move(gate)) {}

    void on_log(const lumen::LogRecord& /*record*/) override {
        if (!waited_) {
            waited_ = true;
            gate_->enter_and_wait();
        }
    }
    void flush() override {}

private:
    std::shared_ptr<Gate> gate_;
    bool waited_ = false;  // touched only by the delivering thread
};

// Counts and prints the checks that failed.
class Report {
public:
    void expect(bool condition, const std::string& what) {
        if (!condition) {
            std::cerr << "MISMATCH: " << what << '\n';
            ++failures_;
        }
    }
    [[nodiscard]] bool passed() const { return failures_ == 0; }

private:
    int32_t failures_ = 0;
};

// Logs one statement per level (Fatal excepted: it terminates) and checks
// which ones arrive.
void check_threshold(Report& report) {
    report.expect(static_cast<int32_t>(lumen::compiled_min_level) == expected_min_level,
                  "lumen::compiled_min_level is " +
                      std::to_string(static_cast<int32_t>(lumen::compiled_min_level)) +
                      ", expected " + std::to_string(expected_min_level));
    auto messages = std::make_shared<std::vector<std::string>>();
    const auto sink = lumen::core().add_sink(std::make_unique<RecordingSink>(messages));
    report.expect(sink.has_value(), "add_sink failed");
    LUMEN_LOG_TRACE("0");
    LUMEN_LOG_DEBUG("1");
    LUMEN_LOG_INFO("2");
    LUMEN_LOG_WARN("3");
    LUMEN_LOG_ERROR("4");
    lumen::core().flush();
    std::string expected;
    for (int32_t level = expected_min_level; level <= 4; ++level) {
        expected += std::to_string(level);
    }
    std::string delivered;
    for (const std::string& message : *messages) {
        delivered += message;
    }
    report.expect(delivered == expected,
                  "delivered levels '" + delivered + "', expected '" + expected + "'");
}

void flood(lumen::Core& core, uint64_t logs, uint64_t metrics, uint64_t progress) {
    for (uint64_t i = 0; i < logs; ++i) {
        lumen::RecordBuilder(core, lumen::LogLevel::Info, std::source_location::current(), "flood");
    }
    for (uint64_t i = 0; i < metrics; ++i) {
        lumen::MetricBuilder(core, "flood", 1.0);
    }
    lumen::ProgressHandle bar(core, "flood", progress);
    for (uint64_t i = 1; i <= progress; ++i) {
        bar.update(i);
    }
}

void expect_refused(Report& report, const char* kind, const lumen::QueueStatistics& stats,
                    uint64_t expected) {
    report.expect(stats.dropped_queue_full == expected,
                  std::string(kind) + " queue refused " + std::to_string(stats.dropped_queue_full) +
                      " records, expected " + std::to_string(expected));
}

// With the dispatcher held, each queue accepts exactly its capacity.
void check_capacities(Report& report) {
    report.expect(lumen::log_queue_capacity == expected_log_capacity, "lumen::log_queue_capacity");
    report.expect(lumen::metric_queue_capacity == expected_metric_capacity,
                  "lumen::metric_queue_capacity");
    report.expect(lumen::progress_queue_capacity == expected_progress_capacity,
                  "lumen::progress_queue_capacity");

    lumen::Core core([](std::string_view /*line*/) {});  // the refusals are expected
    auto gate = std::make_shared<Gate>();
    report.expect(core.add_sink(std::make_unique<GateSink>(gate)).has_value(), "add_sink failed");
    lumen::RecordBuilder(core, lumen::LogLevel::Info, std::source_location::current(), "hold");
    gate->wait_until_entered();
    flood(core, expected_log_capacity + overflow, expected_metric_capacity + overflow,
          expected_progress_capacity + overflow);
    const lumen::CoreStatistics stats = core.statistics();
    gate->open();
    core.flush();
    expect_refused(report, "log", stats.logs, overflow);
    expect_refused(report, "metric", stats.metrics, overflow);
    expect_refused(report, "progress", stats.progress, overflow);
}

}  // namespace

int main() {
    Report report;
    check_threshold(report);
    check_capacities(report);
    if (!report.passed()) {
        return 1;
    }
    std::cout << "consumer: threshold " << expected_min_level << ", capacities "
              << expected_log_capacity << '/' << expected_metric_capacity << '/'
              << expected_progress_capacity << " as built\n";
    return 0;
}
