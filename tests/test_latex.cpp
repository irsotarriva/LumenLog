#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "lumen/core.h"
#include "lumen/predicate.h"
#include "lumen/sink.h"

namespace lumen {
namespace {

struct LatexCaptureSink : public Sink {
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

class LatexTest : public ::testing::Test {
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

TEST_F(LatexTest, LatexTagRecognized) {
    auto sink = std::make_unique<LatexCaptureSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), always());

    LOG_INFO("E = mc^2").tag("latex", "true");

    wait_flush();

    ASSERT_GE(raw->logs.size(), 1);
    EXPECT_EQ(raw->logs[0].tags.find("latex"), "true");
    EXPECT_EQ(raw->logs[0].message, "E = mc^2");
}

TEST_F(LatexTest, LatexTagAbsentByDefault) {
    auto sink = std::make_unique<LatexCaptureSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), always());

    LOG_INFO("plain text");

    wait_flush();

    ASSERT_GE(raw->logs.size(), 1);
    EXPECT_TRUE(raw->logs[0].tags.find("latex").empty());
}

TEST_F(LatexTest, LatexTagOnMetric) {
    auto sink = std::make_unique<LatexCaptureSink>();
    register_sink(std::move(sink), always());

    {
        MetricBuilder builder(core().metric_buffer(), "math_metric", 3.14159);
        builder.tag("latex", "true");
    }

    wait_flush();
    SUCCEED();
}

TEST_F(LatexTest, LatexSinkFiltersLatexRecords) {
    auto latex_sink = std::make_unique<LatexCaptureSink>();
    auto* latex_raw = latex_sink.get();
    register_sink(std::move(latex_sink), tag_equals("latex", "true"));

    auto plain_sink = std::make_unique<LatexCaptureSink>();
    auto* plain_raw = plain_sink.get();
    register_sink(std::move(plain_sink), always());

    LOG_INFO("math content").tag("latex", "true");
    LOG_INFO("plain content");

    wait_flush();

    EXPECT_GE(latex_raw->logs.size(), 1);
    EXPECT_EQ(latex_raw->logs[0].message, "math content");

    EXPECT_GE(plain_raw->logs.size(), 2);
}

TEST_F(LatexTest, LatexTagCombinesWithOtherTags) {
    auto sink = std::make_unique<LatexCaptureSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), always());

    LOG_INFO("formula").tag("latex", "true").tag("section", "math");

    wait_flush();

    ASSERT_GE(raw->logs.size(), 1);
    EXPECT_EQ(raw->logs[0].tags.find("latex"), "true");
    EXPECT_EQ(raw->logs[0].tags.find("section"), "math");
}

}  // namespace
}  // namespace lumen
