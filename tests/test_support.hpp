#ifndef LUMEN_TESTS_TEST_SUPPORT_HPP
#define LUMEN_TESTS_TEST_SUPPORT_HPP

// Shared by the test files: a sink that records what it receives, and
// fixtures that give each test its own Core or a clean global one.

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lumen/context.hpp"
#include "lumen/core.hpp"
#include "lumen/predicate.hpp"
#include "lumen/record.hpp"
#include "lumen/record_builder.hpp"
#include "lumen/sink.hpp"

namespace lumen::test {

// What a CaptureSink received. Shared between the sink (owned by a Core) and
// the test, so the test can read it after the sink is removed or the Core is
// gone. Records own their strings, so the copies stay valid.
class Captured {
public:
    void add(const LogRecord& r) {
        {
            const std::scoped_lock lock(mutex_);
            logs_.push_back(r);
        }
        arrived_.notify_all();
    }

    // Waits until at least `count` log records have arrived without anyone
    // calling flush(), or until `timeout`. Returns whether they arrived.
    [[nodiscard]] bool wait_for_logs(std::size_t count, std::chrono::milliseconds timeout) const {
        std::unique_lock lock(mutex_);
        return arrived_.wait_for(lock, timeout, [&] { return logs_.size() >= count; });
    }
    void add(const MetricRecord& r) {
        const std::scoped_lock lock(mutex_);
        metrics_.push_back(r);
    }
    void add(const ProgressRecord& r) {
        const std::scoped_lock lock(mutex_);
        progress_.push_back(r);
    }
    void add_flush() {
        const std::scoped_lock lock(mutex_);
        ++flushes_;
    }

    [[nodiscard]] std::vector<LogRecord> logs() const {
        const std::scoped_lock lock(mutex_);
        return logs_;
    }
    [[nodiscard]] std::vector<MetricRecord> metrics() const {
        const std::scoped_lock lock(mutex_);
        return metrics_;
    }
    [[nodiscard]] std::vector<ProgressRecord> progress() const {
        const std::scoped_lock lock(mutex_);
        return progress_;
    }
    [[nodiscard]] std::vector<std::string> messages() const {
        const std::scoped_lock lock(mutex_);
        std::vector<std::string> out;
        for (const LogRecord& r : logs_)
            out.emplace_back(r.message);
        return out;
    }
    [[nodiscard]] std::size_t flushes() const {
        const std::scoped_lock lock(mutex_);
        return flushes_;
    }

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable arrived_;  // waits on mutex_
    std::vector<LogRecord> logs_;              // guarded by mutex_
    std::vector<MetricRecord> metrics_;        // guarded by mutex_
    std::vector<ProgressRecord> progress_;     // guarded by mutex_
    std::size_t flushes_ = 0;                  // guarded by mutex_
};

class CaptureSink : public Sink {
public:
    explicit CaptureSink(std::shared_ptr<Captured> captured) : captured_(std::move(captured)) {}
    void on_log(const LogRecord& r) override { captured_->add(r); }
    void on_metric(const MetricRecord& r) override { captured_->add(r); }
    void on_progress(const ProgressRecord& r) override { captured_->add(r); }
    void flush() override { captured_->add_flush(); }

private:
    std::shared_ptr<Captured> captured_;
};

// Registers a CaptureSink on `core` and returns what it captures.
[[nodiscard]] inline std::shared_ptr<Captured> capture(Core& core, Predicate predicate = always()) {
    auto captured = std::make_shared<Captured>();
    const auto id = core.add_sink(std::make_unique<CaptureSink>(captured), std::move(predicate));
    EXPECT_TRUE(id.has_value());
    return captured;
}

// The value of `key` on `tags`, or nullopt.
[[nodiscard]] inline std::optional<std::string> tag_of(const RecordTags& tags,
                                                       std::string_view key) {
    if (!tags.contains(key)) return std::nullopt;
    return std::string(tags.find(key));
}

// All tags, in record order, as owned pairs (for exact comparisons).
[[nodiscard]] inline std::vector<std::pair<std::string, std::string>> tags_of(
    const RecordTags& tags) {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& e : tags)
        out.emplace_back(e.key, e.value);
    return out;
}

// A log statement on a given Core, as the macros write it for core().
inline RecordBuilder log_to(Core& core, std::string_view message, LogLevel level = LogLevel::Info,
                            std::source_location loc = std::source_location::current()) {
    return RecordBuilder(core, level, loc, message);
}

// Clears the process tags and the calling thread's tags on entry and on exit,
// so a test neither sees nor leaves ambient tags.
class CleanContext {
public:
    CleanContext() { clear(); }
    ~CleanContext() { clear(); }
    CleanContext(const CleanContext&) = delete;
    CleanContext& operator=(const CleanContext&) = delete;
    CleanContext(CleanContext&&) = delete;
    CleanContext& operator=(CleanContext&&) = delete;

private:
    static void clear() {
        clear_process_tags();
        clear_thread_tags();
    }
};

// A test with its own Core (diagnostics captured instead of printed), a
// capture sink on it, and a clean ambient context.
class LocalCoreTest : public ::testing::Test {
protected:
    [[nodiscard]] std::vector<std::string> diagnostics() const {
        const std::scoped_lock lock(diagnostics_->mutex);
        return diagnostics_->lines;
    }

    struct Diagnostics {
        std::mutex mutex;
        std::vector<std::string> lines;  // guarded by mutex
    };
    std::shared_ptr<Diagnostics> diagnostics_ = std::make_shared<Diagnostics>();
    CleanContext context_;
    Core core_{[d = diagnostics_](std::string_view line) {
        const std::scoped_lock lock(d->mutex);
        d->lines.emplace_back(line);
    }};
    std::shared_ptr<Captured> captured_ = capture(core_);
};

// A test that has to go through the global core() (the macros): registers a
// capture sink there and removes it afterwards, with a clean ambient context
// before and after. Records left over from earlier tests are flushed out
// before the sink is registered.
class GlobalCoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        core().flush();
        captured_ = std::make_shared<Captured>();
        const auto id = core().add_sink(std::make_unique<CaptureSink>(captured_), always());
        ASSERT_TRUE(id.has_value());
        sink_id_ = *id;
    }
    void TearDown() override {
        core().flush();
        EXPECT_TRUE(core().remove_sink(sink_id_).has_value());
    }

    CleanContext context_;
    std::shared_ptr<Captured> captured_;
    SinkId sink_id_ = 0;
};

}  // namespace lumen::test

#endif
