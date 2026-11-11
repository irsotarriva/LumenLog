// DashboardSink (built with LUMEN_ENABLE_DASHBOARD only): its checks at
// creation, the terminal's alternate screen, frames, and the fatal path.
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

#include "lumen/dashboard_sink.hpp"
#include "lumen/error.hpp"
#include "sink_test_support.hpp"
#include "test_support.hpp"

namespace lumen {
namespace {

using test::log_record;
using test::metric_record;
using test::progress_record;

constexpr std::string_view enter_screen = "\x1b[?1049h\x1b[?25l";
constexpr std::string_view leave_screen = "\x1b[?25h\x1b[?1049l";

class DashboardSinkTest : public ::testing::Test {
protected:
    // Draws into a string stream as if it were a 60x12 terminal; log lines without time.
    [[nodiscard]] DashboardSink::Config _config() {
        DashboardSink::Config config;
        config.output = text_;
        config.assume_terminal = true;
        config.width = 60;
        config.height = 12;
        config.time_format = "";
        config.metrics = {"loss"};
        config.refresh_interval = std::chrono::hours{1};  // frames only on flush()
        config.diagnostics = {};
        return config;
    }

    [[nodiscard]] std::string _output() const { return text_->str(); }

    [[nodiscard]] std::unique_ptr<DashboardSink> _sink() {
        auto sink = DashboardSink::create(_config());
        EXPECT_TRUE(sink.has_value());
        return sink ? std::move(*sink) : nullptr;
    }

private:
    std::shared_ptr<std::ostringstream> text_ = std::make_shared<std::ostringstream>();
};

TEST_F(DashboardSinkTest, CreateFailsWithNotATerminalUnlessTheCallerAssumesOne) {
    DashboardSink::Config config = _config();
    config.assume_terminal = false;
    const auto sink = DashboardSink::create(std::move(config));
    ASSERT_FALSE(sink.has_value());
    EXPECT_EQ(sink.error(), LumenError::NotATerminal);
    EXPECT_EQ(_output(), "");  // nothing written, the screen untouched
}

TEST_F(DashboardSinkTest, CreateRejectsARefreshIntervalBelowOneMillisecondAndABadTimeFormat) {
    DashboardSink::Config fast = _config();
    fast.refresh_interval = std::chrono::milliseconds{0};
    const auto too_fast = DashboardSink::create(std::move(fast));
    ASSERT_FALSE(too_fast.has_value());
    EXPECT_EQ(too_fast.error(), LumenError::InvalidSinkConfig);

    DashboardSink::Config bad_time = _config();
    bad_time.time_format = "%Q";
    const auto bad = DashboardSink::create(std::move(bad_time));
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error(), LumenError::InvalidSinkConfig);
}

TEST_F(DashboardSinkTest, EntersTheAlternateScreenAtCreationAndLeavesItAtDestruction) {
    {
        const auto sink = _sink();
        ASSERT_NE(sink, nullptr);
        EXPECT_TRUE(_output().starts_with(enter_screen));
    }
    EXPECT_TRUE(_output().ends_with(leave_screen));
}

TEST_F(DashboardSinkTest, FlushWritesAFrameWithTheLogLineTheMetricAndTheBar) {
    const auto sink = _sink();
    ASSERT_NE(sink, nullptr);
    sink->on_log(log_record("hello there"));
    sink->on_metric(metric_record("loss", 0.5));
    sink->on_progress(progress_record("files", 3, 10));
    sink->flush();
    const std::string frame = _output().substr(enter_screen.size());
    EXPECT_TRUE(frame.starts_with("\x1b[H"));
    EXPECT_NE(frame.find("INFO  hello there"), std::string::npos);
    EXPECT_NE(frame.find("loss"), std::string::npos);
    EXPECT_NE(frame.find("0.5"), std::string::npos);
    EXPECT_NE(frame.find("files"), std::string::npos);
    EXPECT_NE(frame.find("3/10"), std::string::npos);
    EXPECT_EQ(sink->frames_written(), 1U);
    EXPECT_EQ(sink->frames_failed(), 0U);
}

TEST_F(DashboardSinkTest, AfterAFatalRecordFlushRestoresTheScreenAndPrintsTheLogLinesBelowIt) {
    const auto sink = _sink();
    ASSERT_NE(sink, nullptr);
    sink->on_log(log_record("before"));
    sink->on_log(log_record("boom", LogLevel::Fatal));
    sink->flush();
    const std::string text = _output();
    const std::size_t restored = text.rfind(leave_screen);
    ASSERT_NE(restored, std::string::npos);
    EXPECT_EQ(text.substr(restored + leave_screen.size()), "INFO  before\nFATAL boom\n");

    const std::size_t length = text.size();
    sink->flush();
    EXPECT_EQ(_output().size(), length);  // nothing is drawn after the fatal record
}

TEST_F(DashboardSinkTest, WriteFailureIsCountedAsALostFrame) {
    const auto failing = test::make_stream_over<test::FailingBuffer>();
    DashboardSink::Config config = _config();
    config.output = test::as_ostream(failing);
    const auto sink = DashboardSink::create(std::move(config));
    ASSERT_TRUE(sink.has_value());
    (*sink)->flush();
    EXPECT_EQ((*sink)->frames_written(), 0U);
    EXPECT_EQ((*sink)->frames_failed(), 2U);  // entering the screen, then the frame
}

TEST(DashboardSinkDeathTest, FatalLogLeavesTheMessageOnTheNormalScreenBeforeTermination) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            Core core;
            DashboardSink::Config config;
            config.assume_terminal = true;  // standard error, as a terminal would be
            config.width = 40;
            config.height = 8;
            config.time_format = "";
            auto sink = DashboardSink::create(std::move(config));
            EXPECT_TRUE(sink.has_value());
            EXPECT_TRUE(core.add_sink(std::move(*sink)).has_value());
            test::log_to(core, "boom", LogLevel::Fatal);
        },
        // Leaving the alternate screen, then the log line on the normal one.
        "\\?1049lFATAL boom\n");
}

}  // namespace
}  // namespace lumen
