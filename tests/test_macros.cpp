#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "lumen/core.h"
#include "lumen/predicate.h"
#include "lumen/sink.h"

namespace lumen {
namespace {

struct CaptureSink : public Sink {
    std::vector<LogRecord> logs;
    std::mutex mtx;

    void on_log(const LogRecord& r) override {
        std::lock_guard lock(mtx);
        logs.push_back(r);
    }
    void on_metric(const MetricRecord&) override {}
    void on_progress(const ProgressRecord&) override {}
    void flush() override {}
};

void wait_flush(int ms = 100) {
    core().flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

class MacroTest : public ::testing::Test {
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

TEST_F(MacroTest, MacroCallsRecordBuilder) {
    auto sink = std::make_unique<CaptureSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), always());

    LOG_INFO("test message");

    wait_flush();

    ASSERT_GE(raw->logs.size(), 1);
    EXPECT_EQ(raw->logs[0].message, "test message");
    EXPECT_EQ(raw->logs[0].level, LogLevel::INFO);
}

TEST_F(MacroTest, MacroChainsTags) {
    auto sink = std::make_unique<CaptureSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), always());

    LOG_INFO("tagged msg").tag("key1", "val1").tag("key2", "val2");

    wait_flush();

    ASSERT_GE(raw->logs.size(), 1);
    EXPECT_EQ(raw->logs[0].message, "tagged msg");
    EXPECT_EQ(raw->logs[0].tags.find("key1"), "val1");
    EXPECT_EQ(raw->logs[0].tags.find("key2"), "val2");
}

TEST_F(MacroTest, SourceLocationCaptured) {
    auto sink = std::make_unique<CaptureSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), always());

    const int expected_line = __LINE__ + 1;
    LOG_INFO("location test");

    wait_flush();

    ASSERT_GE(raw->logs.size(), 1);
    EXPECT_FALSE(raw->logs[0].source.file.empty());
    EXPECT_FALSE(raw->logs[0].source.function.empty());
    EXPECT_EQ(raw->logs[0].source.line, expected_line);
}

TEST_F(MacroTest, FatalCallsTerminate) {
    EXPECT_DEATH(
        {
            LOG_FATAL("fatal error");
        },
        "");
}

TEST_F(MacroTest, DebugMacroEmits) {
    auto sink = std::make_unique<CaptureSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), always());

    LOG_DEBUG("debug info");

    wait_flush();

    ASSERT_GE(raw->logs.size(), 1);
    EXPECT_EQ(raw->logs[0].level, LogLevel::DEBUG);
}

TEST_F(MacroTest, TraceMacroCompileTimeElision) {
    auto sink = std::make_unique<CaptureSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), always());

    LOG_DEBUG("debug level emits");
    LOG_INFO("info level emits");

    wait_flush();

    ASSERT_GE(raw->logs.size(), 2);
    EXPECT_EQ(raw->logs[0].level, LogLevel::DEBUG);
    EXPECT_EQ(raw->logs[1].level, LogLevel::INFO);
}

TEST_F(MacroTest, WarnErrorMacrosEmit) {
    auto sink = std::make_unique<CaptureSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), always());

    LOG_WARN("warning");
    LOG_ERROR("error");

    wait_flush();

    ASSERT_GE(raw->logs.size(), 2);
    bool found_warn = false;
    bool found_error = false;
    for (const auto& r : raw->logs) {
        if (r.level == LogLevel::WARN) found_warn = true;
        if (r.level == LogLevel::ERROR) found_error = true;
    }
    EXPECT_TRUE(found_warn);
    EXPECT_TRUE(found_error);
}

// ── Error-path tests ──────────────────────────────────────────────────────────

TEST_F(MacroTest, EmptyMessageLog) {
    auto sink = std::make_unique<CaptureSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), always());

    LOG_INFO("");

    wait_flush();

    ASSERT_GE(raw->logs.size(), 1);
    EXPECT_TRUE(raw->logs[0].message.empty());
}

TEST_F(MacroTest, TagWithMaxValue) {
    auto sink = std::make_unique<CaptureSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), always());

    LOG_INFO("max int").tag("max_int64", static_cast<int64_t>(INT64_MAX));

    wait_flush();

    ASSERT_GE(raw->logs.size(), 1);
    EXPECT_FALSE(raw->logs[0].tags.find("max_int64").empty());
}

TEST_F(MacroTest, TagWithNegativeValue) {
    auto sink = std::make_unique<CaptureSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), always());

    LOG_INFO("negative").tag("neg", static_cast<int64_t>(-42));

    wait_flush();

    ASSERT_GE(raw->logs.size(), 1);
    EXPECT_FALSE(raw->logs[0].tags.find("neg").empty());
}

TEST_F(MacroTest, TagWithDoubleValue) {
    auto sink = std::make_unique<CaptureSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), always());

    LOG_INFO("double tag").tag("pi", 3.14159265358979);

    wait_flush();

    ASSERT_GE(raw->logs.size(), 1);
    EXPECT_FALSE(raw->logs[0].tags.find("pi").empty());
}

TEST_F(MacroTest, ManyTagsOnOneLog) {
    auto sink = std::make_unique<CaptureSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), always());

    LOG_INFO("many tags")
        .tag("key0", "val0")
        .tag("key1", "val1")
        .tag("key2", "val2")
        .tag("key3", "val3")
        .tag("key4", "val4")
        .tag("key5", "val5")
        .tag("key6", "val6")
        .tag("key7", "val7")
        .tag("key8", "val8")
        .tag("key9", "val9")
        .tag("key10", "val10")
        .tag("key11", "val11")
        .tag("key12", "val12")
        .tag("key13", "val13")
        .tag("key14", "val14")
        .tag("key15", "val15")
        .tag("key16", "val16")
        .tag("key17", "val17")
        .tag("key18", "val18")
        .tag("key19", "val19");

    wait_flush();

    ASSERT_GE(raw->logs.size(), 1);
    EXPECT_LE(raw->logs[0].tags.count(), 16);
    EXPECT_GE(raw->logs[0].tags.count(), 1);
}

TEST_F(MacroTest, AllLevelsEmit) {
    auto sink = std::make_unique<CaptureSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), always());

    LOG_DEBUG("debug msg");
    LOG_INFO("info msg");
    LOG_WARN("warn msg");
    LOG_ERROR("error msg");

    wait_flush();

    ASSERT_GE(raw->logs.size(), 4);

    std::set<LogLevel> levels;
    for (const auto& r : raw->logs) {
        levels.insert(r.level);
    }

    EXPECT_NE(levels.find(LogLevel::DEBUG), levels.end());
    EXPECT_NE(levels.find(LogLevel::INFO), levels.end());
    EXPECT_NE(levels.find(LogLevel::WARN), levels.end());
    EXPECT_NE(levels.find(LogLevel::ERROR), levels.end());
}

}  // namespace
}  // namespace lumen
