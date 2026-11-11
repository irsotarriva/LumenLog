// DashboardSink (built with LUMEN_ENABLE_DASHBOARD only): its checks at
// creation, the terminal's alternate screen, frames, and the fatal path.
#include <gtest/gtest.h>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#ifndef PLATFORM_WINDOWS
#include <unistd.h>
#endif

#include "lumen/dashboard_sink.hpp"
#include "lumen/error.hpp"
#include "lumen/predicate.hpp"
#include "lumen/thread_clock.hpp"
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
    ProgressRecord bar = progress_record("files", 3, 10);
    bar.timestamp_ns = now_ns();  // a bar idle for 60 s is removed before the frame
    sink->on_progress(bar);
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

// Frames are drawn on the sink's thread while records keep arriving; the
// frame that flush() waits for shows the last of them.
TEST_F(DashboardSinkTest, RecordsArrivingWhileFramesAreDrawnAllReachTheFrameWrittenByFlush) {
    DashboardSink::Config config = _config();
    config.refresh_interval = std::chrono::milliseconds{1};
    const auto created = DashboardSink::create(std::move(config));
    ASSERT_TRUE(created.has_value());
    DashboardSink& sink = **created;
    for (uint32_t i = 0; i < 300; ++i) {
        const std::string message = "line " + std::to_string(i);
        sink.on_log(log_record(message));
        sink.on_metric(metric_record("loss", 2.5 * static_cast<double>(i)));
    }
    sink.flush();
    const std::string text = _output();
    const std::size_t last_frame = text.rfind("\x1b[H");
    ASSERT_NE(last_frame, std::string::npos);
    EXPECT_NE(text.find("INFO  line 299", last_frame), std::string::npos);
    EXPECT_NE(text.find("747.5", last_frame), std::string::npos);  // the last metric value
    EXPECT_GE(sink.frames_written(), 1U);
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

// The dashboard on standard error (assumed to be a terminal), registered in
// `core` with `predicate`, holding one log line.
void add_dashboard_on_standard_error(Core& core, Predicate predicate = always()) {
    DashboardSink::Config config;
    config.assume_terminal = true;
    config.width = 40;
    config.height = 8;
    config.time_format = "";
    config.diagnostics = {};
    auto sink = DashboardSink::create(std::move(config));
    ASSERT_TRUE(sink.has_value());
    ASSERT_TRUE(core.add_sink(std::move(*sink), std::move(predicate)).has_value());
    test::log_to(core, "shown").tag("view", "dashboard");
    core.flush();
}

// "ESC[?25h ESC[?1049l": the cursor shown, the alternate screen left.
constexpr std::string_view restored_pattern = R"(\?25h.\[\?1049l)";

TEST(DashboardSinkDeathTest, FatalRecordFilteredOutByThePredicateStillRestoresTheTerminal) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            Core core;
            add_dashboard_on_standard_error(core, tag_equals("view", "dashboard"));
            test::log_to(core, "not for the dashboard", LogLevel::Fatal);
        },
        // The terminal restored, then the lines the dashboard held.
        std::string(restored_pattern) + "INFO  shown\n");
}

TEST(DashboardSinkDeathTest, TerminateForAnyReasonRestoresTheTerminal) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            Core core;
            add_dashboard_on_standard_error(core);
            std::terminate();
        },
        std::string(restored_pattern) + "INFO  shown\n");
}

TEST(DashboardSinkDeathTest, ExitWhileTheSinkIsAliveRestoresTheTerminal) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            Core core;  // its destructor does not run on std::exit
            add_dashboard_on_standard_error(core);
            std::exit(3);  // NOLINT(concurrency-mt-unsafe): the death test is one thread
        },
        ::testing::ExitedWithCode(3), std::string(restored_pattern));
}

#ifndef PLATFORM_MACOS
TEST(DashboardSinkDeathTest, QuickExitWhileTheSinkIsAliveRestoresTheTerminal) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            Core core;
            add_dashboard_on_standard_error(core);
            std::quick_exit(4);
        },
        ::testing::ExitedWithCode(4), std::string(restored_pattern));
}
#endif

#ifndef PLATFORM_WINDOWS
extern "C" void restore_and_exit(int /*signal*/) {
    DashboardSink::restore_terminal_after_signal();
    _exit(5);
}

TEST(DashboardSinkDeathTest, SignalHandlerCanRestoreTheTerminalBeforeEndingTheProcess) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            Core core;
            add_dashboard_on_standard_error(core);
            static_cast<void>(std::signal(SIGTERM, &restore_and_exit));
            static_cast<void>(std::raise(SIGTERM));
        },
        ::testing::ExitedWithCode(5), std::string(restored_pattern));
}
#endif

TEST_F(DashboardSinkTest, RestoreAfterSignalWritesOnlyWhileADashboardOnStandardErrorIsAlive) {
    const auto on_stream = _sink();  // a caller's stream: not restored by the function
    ASSERT_NE(on_stream, nullptr);
    ::testing::internal::CaptureStderr();
    DashboardSink::restore_terminal_after_signal();
    {
        DashboardSink::Config config = _config();
        config.output = nullptr;
        const auto on_standard_error = DashboardSink::create(std::move(config));
        ASSERT_TRUE(on_standard_error.has_value());
        DashboardSink::restore_terminal_after_signal();
    }
    DashboardSink::restore_terminal_after_signal();
    const std::string written = ::testing::internal::GetCapturedStderr();
    // Entering, the function's restore, the destructor's; nothing before or after.
    EXPECT_EQ(written,
              std::string(enter_screen) + std::string(leave_screen) + std::string(leave_screen));
}

TEST_F(DashboardSinkTest, LargestRefreshIntervalIsAcceptedAndFlushStillDraws) {
    DashboardSink::Config config = _config();
    config.refresh_interval = std::chrono::milliseconds::max();
    const auto sink = DashboardSink::create(std::move(config));
    ASSERT_TRUE(sink.has_value());
    (*sink)->on_log(log_record("still drawn"));
    (*sink)->flush();
    EXPECT_EQ((*sink)->frames_written(), 1U);
    EXPECT_NE(_output().find("INFO  still drawn"), std::string::npos);
}

#ifndef PLATFORM_WINDOWS
// Without a configured size, each frame measures the terminal the dashboard
// draws on (standard error here, 30 x 7), not standard output.
TEST_F(DashboardSinkTest, FrameHasTheSizeOfTheTerminalOnStandardError) {
    test::StandardErrorOnPty pty(30, 7);
    DashboardSink::Config config = _config();
    config.output = nullptr;
    config.width = 0;
    config.height = 0;
    const auto sink = DashboardSink::create(std::move(config));
    ASSERT_TRUE(sink.has_value());
    (*sink)->flush();
    const std::string text = pty.take_output();
    const std::size_t frame = text.rfind("\x1b[H");
    ASSERT_NE(frame, std::string::npos);
    std::size_t rows = 0;
    for (std::size_t pos = text.find("\x1b[K", frame); pos != std::string::npos;
         pos = text.find("\x1b[K", pos + 1)) {
        ++rows;  // every row of a frame ends by clearing the rest of the line
    }
    EXPECT_EQ(rows, 7U);
}
#endif

}  // namespace
}  // namespace lumen
