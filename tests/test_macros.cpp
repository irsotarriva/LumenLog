// The logging macros, which always go to the global core(). Each test starts
// from a clean context and a fresh capture sink (GlobalCoreTest). Thresholds
// other than this target's are covered by test_elision.cpp.
#include <gtest/gtest.h>

#include <concepts>
#include <iostream>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#include "lumen/macros.hpp"
#include "test_support.hpp"

namespace lumen {
namespace {

using test::GlobalCoreTest;
using test::StderrSink;
using test::tag_of;

template <typename T>
    requires std::is_arithmetic_v<T> || std::convertible_to<T, std::string_view>
void log_from_a_function_template(const T& value) {
    LOG_INFO("from a function template {}", value);
}

class MacroTest : public GlobalCoreTest {
protected:
    [[nodiscard]] std::vector<LogRecord> _logs() {
        core().flush();
        return _captured->logs();
    }
};

TEST_F(MacroTest, EachEnabledLevelMacroEmitsOneRecordAtItsLevel) {
    LUMEN_LOG_DEBUG("debug");
    LUMEN_LOG_INFO("info");
    LUMEN_LOG_WARN("warn");
    LUMEN_LOG_ERROR("error");
    const auto records = _logs();
    ASSERT_EQ(records.size(), 4U);
    EXPECT_EQ(records[0].level, LogLevel::Debug);
    EXPECT_EQ(records[1].level, LogLevel::Info);
    EXPECT_EQ(records[2].level, LogLevel::Warn);
    EXPECT_EQ(records[3].level, LogLevel::Error);
    EXPECT_EQ(records[3].message, "error");
}

TEST_F(MacroTest, ShortNamesEmitLikeThePrefixedOnes) {
    LOG_INFO("short info");
    LOG_ERROR("short error");
    const auto records = _logs();
    ASSERT_EQ(records.size(), 2U);
    EXPECT_EQ(records[0].level, LogLevel::Info);
    EXPECT_EQ(records[1].level, LogLevel::Error);
}

TEST_F(MacroTest, TraceIsElidedAtTheDefaultThresholdAndItsArgumentsAreNotEvaluated) {
    static_assert(compiled_min_level == LogLevel::Debug,
                  "this target is built with the default threshold");
    int evaluated = 0;
    auto side_effect = [&evaluated] { return ++evaluated; };
    LUMEN_LOG_TRACE("trace {}", side_effect());
    LOG_TRACE("trace {}", side_effect());
    EXPECT_EQ(evaluated, 0);
    EXPECT_TRUE(_logs().empty());
}

TEST_F(MacroTest, MacroRecordsTheCallersSourceLocationAndClass) {
    struct Caller {
        static std::source_location log_here() {
            const std::source_location here = std::source_location::current();
            LUMEN_LOG_INFO("located");
            return here;
        }
    };
    const std::source_location here = Caller::log_here();
    const auto records = _logs();
    ASSERT_EQ(records.size(), 1U);
    EXPECT_EQ(records[0].source.line, here.line() + 1);
    EXPECT_EQ(records[0].source.file, here.file_name());
    EXPECT_EQ(records[0].source.class_name, "Caller");
}

TEST_F(MacroTest, FormatArgumentsAreFormattedAndASingleArgumentIsVerbatim) {
    LOG_INFO("cluster energy {:.2f} GeV in {} hits", 12.3456, 7).tag("run", 3);
    LOG_WARN("single argument is verbatim: {braces} stay");
    const auto records = _logs();
    ASSERT_EQ(records.size(), 2U);
    EXPECT_EQ(records[0].message, "cluster energy 12.35 GeV in 7 hits");
    EXPECT_EQ(tag_of(records[0].tags, "run"), "3");
    EXPECT_EQ(records[1].message, "single argument is verbatim: {braces} stay");
}

TEST_F(MacroTest, ChainedTagsAreAllOnTheRecord) {
    LOG_INFO("tags").tag("a", "1").tag("b", 2).tag("c", true);
    const auto records = _logs();
    ASSERT_EQ(records.size(), 1U);
    EXPECT_EQ(tag_of(records[0].tags, "a"), "1");
    EXPECT_EQ(tag_of(records[0].tags, "b"), "2");
    EXPECT_EQ(tag_of(records[0].tags, "c"), "true");
}

// The missing braces are what this test is about.
// NOLINTBEGIN(readability-braces-around-statements)
TEST_F(MacroTest, ElseAfterAnEnabledMacroBindsToTheCallersIf) {
    int else_taken = 0;
    for (const bool condition : {false, true}) {
        if (condition)
            LOG_INFO("taken");
        else
            ++else_taken;
    }
    EXPECT_EQ(else_taken, 1);
    const auto records = _logs();
    ASSERT_EQ(records.size(), 1U);
    EXPECT_EQ(records[0].message, "taken");
}
// NOLINTEND(readability-braces-around-statements)

TEST_F(MacroTest, MacroWorksInsideALambdaAGenericLambdaAndAFunctionTemplate) {
    auto in_lambda = [] { LOG_INFO("from lambda"); };
    in_lambda();
    [](auto value) { LOG_INFO("from generic lambda {}", value); }(5);
    log_from_a_function_template(7);
    log_from_a_function_template(std::string("text"));
    const auto records = _logs();
    ASSERT_EQ(records.size(), 4U);
    EXPECT_EQ(records[0].message, "from lambda");
    EXPECT_EQ(records[1].message, "from generic lambda 5");
    EXPECT_EQ(records[2].message, "from a function template 7");
    EXPECT_EQ(records[3].message, "from a function template text");
}

TEST_F(MacroTest, FormatFailureInsideTheMacroIsLoggedInsteadOfThrown) {
    const int negative_width = -1;
    LUMEN_LOG_WARN("width {:{}}", 5, negative_width).tag("kept", "yes");
    const auto records = _logs();
    ASSERT_EQ(records.size(), 1U);
    EXPECT_TRUE(records[0].message.starts_with("formatting failed: ")) << records[0].message;
    EXPECT_TRUE(records[0].message.ends_with("; format string: width {:{}}")) << records[0].message;
    EXPECT_EQ(records[0].level, LogLevel::Warn);
    EXPECT_EQ(tag_of(records[0].tags, "kept"), "yes");
}

TEST_F(MacroTest, NullCStringMessageIsLoggedAsNull) {
    const char* const message = nullptr;
    LUMEN_LOG_INFO(message);
    const auto records = _logs();
    ASSERT_EQ(records.size(), 1U);
    EXPECT_EQ(records[0].message, "(null)");
}

TEST_F(MacroTest, RecordFromAnExitedThreadKeepsThatThreadsTags) {
    std::thread worker([] {
        ASSERT_TRUE(set_thread_tag("worker", std::string(40, 'w')));
        LOG_INFO("from worker");
    });
    worker.join();
    const auto records = _logs();
    ASSERT_EQ(records.size(), 1U);
    EXPECT_EQ(tag_of(records[0].tags, "worker"), std::string(40, 'w'));
}

TEST_F(MacroTest, MetricAndProgressHelpersUseTheGlobalCore) {
    lumen::metric("loss", 0.5).tag("epoch", 1);
    lumen::progress("bar", 2).finish();
    core().flush();
    const auto metrics = _captured->metrics();
    const auto progress = _captured->progress();
    ASSERT_EQ(metrics.size(), 1U);
    ASSERT_EQ(progress.size(), 1U);
    EXPECT_EQ(tag_of(metrics[0].tags, "epoch"), "1");
    EXPECT_EQ(progress[0].current, 2U);
}

TEST(MacroCoreTest, CoreReturnsTheSameObjectEveryTime) {
    EXPECT_EQ(&core(), &core());
}

TEST(MacroFatalDeathTest, FatalMacroDeliversItsRecordFlushesAndTerminates) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            EXPECT_TRUE(core().add_sink(std::make_unique<StderrSink>()));
            LUMEN_LOG_INFO("before");
            LUMEN_LOG_FATAL("fatal {}", 42);
        },
        "delivered:before\ndelivered:fatal 42\nflushed");
}

}  // namespace
}  // namespace lumen
