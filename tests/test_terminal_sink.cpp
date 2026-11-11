// TerminalSink: the exact text it writes, colour, its configuration checks,
// and its writer thread (queue bound, failures, isolation from other sinks).
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "lumen/error.hpp"
#include "lumen/terminal_sink.hpp"
#include "sink_test_support.hpp"
#include "test_support.hpp"

namespace lumen {
namespace {

using test::log_record;
using test::metric_record;
using test::progress_record;

// Diagnostic lines a sink reported. The handler shares ownership, so a sink
// that outlives the test body (one owned by a Core) never reports into a
// destroyed object.
class Reported {
public:
    [[nodiscard]] DiagnosticHandler handler() const {
        return [state = state_](std::string_view line) {
            const std::scoped_lock lock(state->mutex);
            state->lines.emplace_back(line);
        };
    }
    [[nodiscard]] std::vector<std::string> all() const {
        const std::scoped_lock lock(state_->mutex);
        return state_->lines;
    }

private:
    struct State {
        std::mutex mutex;
        std::vector<std::string> lines;  // guarded by mutex
    };
    std::shared_ptr<State> state_ = std::make_shared<State>();
};

// Opens the gate when the test body ends, however it ends, so that a failed
// assertion does not leave the sink's writer blocked while the Core flushes.
class OpenAtExit {
public:
    explicit OpenAtExit(test::GateBuffer& gate) : gate_(gate) {}
    ~OpenAtExit() { gate_.open(); }
    OpenAtExit(const OpenAtExit&) = delete;
    OpenAtExit& operator=(const OpenAtExit&) = delete;
    OpenAtExit(OpenAtExit&&) = delete;
    OpenAtExit& operator=(OpenAtExit&&) = delete;

private:
    test::GateBuffer& gate_;
};

class TerminalSinkTest : public ::testing::Test {
protected:
    // A config writing into a string stream, without colour and without a time column.
    [[nodiscard]] TerminalSink::Config _config() {
        TerminalSink::Config config;
        config.output = text_;
        config.color = ColorMode::Never;
        config.time_format = "";
        config.diagnostics = reported_.handler();
        return config;
    }

    // What the sinks made by _config() wrote, and the diagnostics they gave.
    [[nodiscard]] std::string _output() const { return text_->str(); }
    [[nodiscard]] std::vector<std::string> _diagnostics() { return reported_.all(); }

    [[nodiscard]] static std::unique_ptr<TerminalSink> _sink(TerminalSink::Config config) {
        auto sink = TerminalSink::create(std::move(config));
        EXPECT_TRUE(sink.has_value());
        return sink ? std::move(*sink) : nullptr;
    }

    // What `config` writes for `record`.
    template <test::AnyRecord Record>
    [[nodiscard]] std::string _written(TerminalSink::Config config, const Record& record) {
        const auto sink = _sink(std::move(config));
        if (!sink) {
            return {};
        }
        test::deliver(*sink, record);
        sink->flush();
        return _output();
    }

private:
    std::shared_ptr<std::ostringstream> text_ = std::make_shared<std::ostringstream>();
    Reported reported_;
};

TEST_F(TerminalSinkTest, LogLineIsTimeLevelMessageThenTheInlineTagsInConfiguredOrder) {
    TerminalSink::Config config = _config();
    config.time_format = "%H:%M:%S.%L";
    config.time_zone = TimeZone::Utc;
    config.inline_tags = {"port", "host", "absent"};
    LogRecord record = log_record("connected");
    EXPECT_TRUE(record.tags.add("host", "db1"));
    EXPECT_TRUE(record.tags.add("other", "hidden"));
    EXPECT_TRUE(record.tags.add("port", "5432"));
    EXPECT_EQ(_written(std::move(config), record),
              "10:00:00.123 INFO  connected port=5432 host=db1\n");
}

TEST_F(TerminalSinkTest, MetricAndProgressLinesCarryTheirMarkers) {
    const auto sink = _sink(_config());
    ASSERT_NE(sink, nullptr);
    sink->on_metric(metric_record("latency_ms", 3.25));
    sink->on_progress(progress_record("files", 3, 10));
    sink->flush();
    EXPECT_EQ(_output(), "METRIC latency_ms=3.25\nPROGRESS files 3/10\n");
}

TEST_F(TerminalSinkTest, ShowSourceAppendsFileAndLineToLogLines) {
    TerminalSink::Config config = _config();
    config.show_source = true;
    EXPECT_EQ(_written(std::move(config), log_record("m", LogLevel::Error)),
              "ERROR m (main.cpp:42)\n");
}

TEST_F(TerminalSinkTest, ColourAlwaysWrapsOnlyTheLevelNameInTheLevelsColour) {
    TerminalSink::Config config = _config();
    config.color = ColorMode::Always;
    const auto sink = _sink(std::move(config));
    ASSERT_NE(sink, nullptr);
    EXPECT_TRUE(sink->colored());
    sink->on_log(log_record("ok", LogLevel::Info));
    sink->on_log(log_record("careful", LogLevel::Warn));
    sink->on_log(log_record("fine print", LogLevel::Trace));
    sink->flush();
    EXPECT_EQ(_output(),
              "\x1b[32mINFO\x1b[0m  ok\n"
              "\x1b[33mWARN\x1b[0m  careful\n"
              "TRACE fine print\n");  // Trace's default colour is the terminal's own
}

TEST_F(TerminalSinkTest, ConfiguredColoursReplaceTheDefaults) {
    TerminalSink::Config config = _config();
    config.color = ColorMode::Always;
    config.colors.at(static_cast<std::size_t>(LogLevel::Info)) = AnsiColor::Blue;
    EXPECT_EQ(_written(std::move(config), log_record("x")), "\x1b[34mINFO\x1b[0m  x\n");
}

TEST_F(TerminalSinkTest, LevelOutsideTheEnumIsUnknownInTheDefaultColour) {
    TerminalSink::Config config = _config();
    config.color = ColorMode::Always;
    EXPECT_EQ(_written(std::move(config), log_record("x", static_cast<LogLevel>(200))),
              "UNKNOWN x\n");
}

TEST_F(TerminalSinkTest, AutoColourOnACallersStreamWritesNoEscapes) {
    TerminalSink::Config config = _config();
    config.color = ColorMode::Auto;
    const auto sink = _sink(std::move(config));
    ASSERT_NE(sink, nullptr);
    EXPECT_FALSE(sink->colored());
    sink->on_log(log_record("plain", LogLevel::Error));
    sink->flush();
    EXPECT_EQ(_output(), "ERROR plain\n");
}

TEST_F(TerminalSinkTest, ControlBytesInTheMessageAreEscapedSoTheyCannotDriveTheTerminal) {
    EXPECT_EQ(_written(_config(), log_record("a\nb\x1b[2Jc")), "INFO  a\\nb\\x1b[2Jc\n");
}

TEST_F(TerminalSinkTest, TimeFormatAcceptsStrftimeConversionsMillisecondsAndNanoseconds) {
    TerminalSink::Config config = _config();
    config.time_format = "[%Y-%m-%d %H:%M:%S.%N|%L%%]";
    config.time_zone = TimeZone::Utc;
    EXPECT_EQ(_written(std::move(config), log_record("t")),
              "[2026-11-11 10:00:00.123456789|123%] INFO  t\n");
}

TEST_F(TerminalSinkTest, CreateRejectsAnInvalidTimeFormatOrAnEmptyQueue) {
    for (const char* pattern : {"%", "%Q", "%Ec", "%H:%"}) {
        TerminalSink::Config config = _config();
        config.time_format = pattern;
        const auto sink = TerminalSink::create(std::move(config));
        ASSERT_FALSE(sink.has_value()) << pattern;
        EXPECT_EQ(sink.error(), LumenError::InvalidSinkConfig) << pattern;
    }
    TerminalSink::Config config = _config();
    config.max_queued_lines = 0;
    const auto sink = TerminalSink::create(std::move(config));
    ASSERT_FALSE(sink.has_value());
    EXPECT_EQ(sink.error(), LumenError::InvalidSinkConfig);
}

TEST_F(TerminalSinkTest, DestroyingTheSinkWritesTheLinesStillQueued) {
    {
        const auto sink = _sink(_config());
        ASSERT_NE(sink, nullptr);
        sink->on_log(log_record("one"));
        sink->on_log(log_record("two"));
    }
    EXPECT_EQ(_output(), "INFO  one\nINFO  two\n");
}

TEST_F(TerminalSinkTest, FailingStreamCountsEachLostLineAndReportsTheFirstFailureOnly) {
    const auto failing = test::make_stream_over<test::FailingBuffer>();
    TerminalSink::Config config = _config();
    config.output = test::as_ostream(failing);
    const auto sink = _sink(std::move(config));
    ASSERT_NE(sink, nullptr);
    for (uint32_t i = 0; i < 3; ++i) {
        sink->on_log(log_record("lost"));
        sink->flush();
    }
    const WriterStatistics stats = sink->statistics();
    EXPECT_EQ(stats.lines_queued, 3U);
    EXPECT_EQ(stats.lines_written, 0U);
    EXPECT_EQ(stats.lines_failed, 3U);
    const std::vector<std::string> reported = _diagnostics();
    ASSERT_EQ(reported.size(), 1U);
    EXPECT_NE(reported.front().find("TerminalSink: write failed"), std::string::npos);
}

class TerminalSinkOnCoreTest : public test::LocalCoreTest {};

TEST_F(TerminalSinkOnCoreTest, RecordsLoggedThroughACoreAreWrittenInOrderWhenFlushReturns) {
    const auto text = std::make_shared<std::ostringstream>();
    TerminalSink::Config config;
    config.output = text;
    config.color = ColorMode::Never;
    config.time_format = "";
    auto sink = TerminalSink::create(std::move(config));
    ASSERT_TRUE(sink.has_value());
    ASSERT_TRUE(_core.add_sink(std::move(*sink)).has_value());
    test::log_to(_core, "first");
    test::log_to(_core, "second", LogLevel::Warn);
    test::log_to(_core, "third", LogLevel::Debug);
    _core.flush();
    EXPECT_EQ(text->str(), "INFO  first\nWARN  second\nDEBUG third\n");
}

// The sink writes into a stream that holds its first write until the test
// opens it. With room for 4 queued lines: record 1 is held at the gate,
// records 2-5 are queued, records 6-10 are dropped. Meanwhile a sink
// registered after it must still receive all ten records without anyone
// calling flush.
TEST_F(TerminalSinkOnCoreTest,
       BlockedOutputDelaysNoOtherSinkAndItsOverflowIsCountedAndReportedOnce) {
    const auto gate = test::make_stream_over<test::GateBuffer>();
    const OpenAtExit open_at_exit(gate->buffer);
    const Reported reported;
    TerminalSink::Config config;
    config.output = test::as_ostream(gate);
    config.color = ColorMode::Never;
    config.time_format = "";
    config.max_queued_lines = 4;
    config.diagnostics = reported.handler();
    auto created = TerminalSink::create(std::move(config));
    ASSERT_TRUE(created.has_value());
    const TerminalSink* terminal = created->get();  // non-owning; the Core owns it
    ASSERT_TRUE(_core.add_sink(std::move(*created)).has_value());
    const auto after = test::capture(_core);

    test::log_to(_core, "1");
    ASSERT_TRUE(after->wait_for_logs(1, std::chrono::seconds(10)));
    gate->buffer.wait_until_entered();
    for (uint32_t i = 2; i <= 10; ++i) {
        test::log_to(_core, std::to_string(i));
    }
    EXPECT_TRUE(after->wait_for_logs(10, std::chrono::seconds(10)));

    const WriterStatistics blocked = terminal->statistics();
    EXPECT_EQ(blocked.lines_queued, 5U);
    EXPECT_EQ(blocked.lines_dropped, 5U);
    EXPECT_EQ(blocked.lines_written, 0U);
    const std::vector<std::string> lines = reported.all();
    ASSERT_EQ(lines.size(), 1U);
    EXPECT_NE(lines.front().find("TerminalSink: output queue full (4 lines)"), std::string::npos);

    gate->buffer.open();
    _core.flush();
    EXPECT_EQ(gate->buffer.text(), "INFO  1\nINFO  2\nINFO  3\nINFO  4\nINFO  5\n");
    const WriterStatistics done = terminal->statistics();
    EXPECT_EQ(done.lines_written, 5U);
    EXPECT_EQ(done.lines_queued, done.lines_written + done.lines_failed);
}

}  // namespace
}  // namespace lumen
