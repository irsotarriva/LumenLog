// Compile-time elision. This file is built once per threshold (see
// tests/CMakeLists.txt) with LUMEN_TEST_EXPECTED_MIN_LEVEL set to it. At
// every threshold, each macro at or above it emits exactly one record at its level, and each
// one below it emits nothing and does not evaluate its arguments.
#ifndef LUMEN_TEST_EXPECTED_MIN_LEVEL
#error "build this file with LUMEN_TEST_EXPECTED_MIN_LEVEL"
#endif

// Whatever threshold the library was configured with, this file tests its own.
#undef LUMEN_MIN_LEVEL_VALUE
#define LUMEN_MIN_LEVEL_VALUE LUMEN_TEST_EXPECTED_MIN_LEVEL

#include <gtest/gtest.h>

#include <concepts>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "lumen/macros.hpp"
#include "test_support.hpp"

namespace lumen {
namespace {

static_assert(std::to_underlying(compiled_min_level) == LUMEN_TEST_EXPECTED_MIN_LEVEL,
              "the threshold this target was built for");

class ElisionTest : public test::GlobalCoreTest {
protected:
    [[nodiscard]] std::vector<LogLevel> _delivered_levels() {
        core().flush();
        std::vector<LogLevel> out;
        for (const LogRecord& r : _captured->logs()) {
            out.push_back(r.level);
        }
        return out;
    }
};

[[nodiscard]] constexpr bool enabled(LogLevel level) {
    // Written as a search, not `level >= threshold`, which GCC flags as
    // always true when the threshold is 0.
    for (int value = LUMEN_TEST_EXPECTED_MIN_LEVEL; value <= 5; ++value) {
        if (static_cast<int>(std::to_underlying(level)) == value) {
            return true;
        }
    }
    return false;
}

TEST_F(ElisionTest, MacrosAtOrAboveTheThresholdEmitAndThoseBelowEmitNothing) {
    LUMEN_LOG_TRACE("trace");
    LUMEN_LOG_DEBUG("debug");
    LUMEN_LOG_INFO("info");
    LUMEN_LOG_WARN("warn");
    LUMEN_LOG_ERROR("error");
    std::vector<LogLevel> expected;
    for (const LogLevel level :
         {LogLevel::Trace, LogLevel::Debug, LogLevel::Info, LogLevel::Warn, LogLevel::Error}) {
        if (enabled(level)) {
            expected.push_back(level);
        }
    }
    EXPECT_EQ(_delivered_levels(), expected);
}

TEST_F(ElisionTest, ArgumentsOfElidedMacrosAreNotEvaluated) {
    int trace = 0;
    int debug = 0;
    int info = 0;
    int info_tag = 0;
    int warn = 0;
    int error = 0;
    LUMEN_LOG_TRACE("{}", ++trace);
    LUMEN_LOG_DEBUG("{}", ++debug);
    LUMEN_LOG_INFO("{}", ++info).tag("n", ++info_tag);
    LUMEN_LOG_WARN("{}", ++warn);
    LUMEN_LOG_ERROR("{}", ++error);
    EXPECT_EQ(trace, enabled(LogLevel::Trace) ? 1 : 0);
    EXPECT_EQ(debug, enabled(LogLevel::Debug) ? 1 : 0);
    EXPECT_EQ(info, enabled(LogLevel::Info) ? 1 : 0);
    EXPECT_EQ(info_tag, enabled(LogLevel::Info) ? 1 : 0);
    EXPECT_EQ(warn, enabled(LogLevel::Warn) ? 1 : 0);
    EXPECT_EQ(error, enabled(LogLevel::Error) ? 1 : 0);
}

// One `if (c) MACRO(...); else ...` per level, run with c false then true.
// Returns how often the else branch ran. The missing braces are the point.
// NOLINTBEGIN(readability-braces-around-statements)
[[nodiscard]] int else_count_trace() {
    int else_taken = 0;
    for (const bool condition : {false, true}) {
        if (condition)
            LUMEN_LOG_TRACE("t");
        else
            ++else_taken;
    }
    return else_taken;
}

[[nodiscard]] int else_count_info() {
    int else_taken = 0;
    for (const bool condition : {false, true}) {
        if (condition)
            LUMEN_LOG_INFO("i");
        else
            ++else_taken;
    }
    return else_taken;
}

[[nodiscard]] int else_count_error() {
    int else_taken = 0;
    for (const bool condition : {false, true}) {
        if (condition)
            LUMEN_LOG_ERROR("e");
        else
            ++else_taken;
    }
    return else_taken;
}
// NOLINTEND(readability-braces-around-statements)

TEST_F(ElisionTest, ElseAfterATraceMacroAsTheBodyOfAnIfBindsToThatIf) {
    EXPECT_EQ(else_count_trace(), 1);
    EXPECT_EQ(_delivered_levels().size(), enabled(LogLevel::Trace) ? 1U : 0U);
}

TEST_F(ElisionTest, ElseAfterAnInfoMacroAsTheBodyOfAnIfBindsToThatIf) {
    EXPECT_EQ(else_count_info(), 1);
    EXPECT_EQ(_delivered_levels().size(), enabled(LogLevel::Info) ? 1U : 0U);
}

TEST_F(ElisionTest, ElseAfterAnErrorMacroAsTheBodyOfAnIfBindsToThatIf) {
    EXPECT_EQ(else_count_error(), 1);
    EXPECT_EQ(_delivered_levels().size(), enabled(LogLevel::Error) ? 1U : 0U);
}

template <typename T>
    requires std::integral<T>
void log_from_template(T value) {
    LUMEN_LOG_INFO("template {}", value);
    LUMEN_LOG_ERROR("template {}", value);
}

TEST_F(ElisionTest, ElisionAppliesInsideAFunctionTemplate) {
    log_from_template(int64_t{1});
    std::vector<LogLevel> expected;
    for (const LogLevel level : {LogLevel::Info, LogLevel::Error}) {
        if (enabled(level)) {
            expected.push_back(level);
        }
    }
    EXPECT_EQ(_delivered_levels(), expected);
}

}  // namespace
}  // namespace lumen
