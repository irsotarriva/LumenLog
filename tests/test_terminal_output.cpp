// The destination of TerminalSink and DashboardSink (src/terminal_output.cpp):
// writing a batch to a stream, colour resolution and the terminal's size.
#include <gtest/gtest.h>

#include <ios>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <sstream>
#include <streambuf>
#include <string>
#include <string_view>
#include <vector>

#include "lumen/terminal_style.hpp"
#include "sink_test_support.hpp"
#include "terminal_output.hpp"

namespace lumen::detail {
namespace {

[[nodiscard]] BatchOutcome write_lines(std::ostream& stream,
                                       const std::vector<std::string>& lines) {
    return write_and_flush(stream, std::span<const std::string>(lines));
}

TEST(WriteAndFlushTest, WritesEveryLineInOrderAndReportsNoFailure) {
    std::ostringstream stream;
    const BatchOutcome outcome = write_lines(stream, {"a\n", "b\n"});
    EXPECT_EQ(outcome.failed_lines, 0U);
    EXPECT_EQ(outcome.failure, "");
    EXPECT_EQ(stream.str(), "a\nb\n");
}

TEST(WriteAndFlushTest, FailingStreamFailsEveryLineOfTheBatchAndIsUsableForTheNext) {
    test::FailOnceBuffer buffer;
    std::ostream stream(&buffer);
    const BatchOutcome failed = write_lines(stream, {"lost\n", "also lost\n"});
    EXPECT_EQ(failed.failed_lines, 2U);
    EXPECT_EQ(failed.failure, "the stream reported an error");
    EXPECT_TRUE(stream.good());
    const BatchOutcome next = write_lines(stream, {"kept\n"});
    EXPECT_EQ(next.failed_lines, 0U);
    EXPECT_NE(buffer.text().find("kept\n"), std::string::npos);
}

TEST(WriteAndFlushTest, StreamThatThrowsOnFailureIsClearedAndTheNextBatchIsWritten) {
    test::FailOnceBuffer buffer;
    std::ostream stream(&buffer);
    stream.exceptions(std::ios::badbit);
    const BatchOutcome failed = write_lines(stream, {"lost\n"});
    EXPECT_EQ(failed.failed_lines, 1U);
    EXPECT_TRUE(failed.failure.starts_with("the stream threw: ")) << failed.failure;
    EXPECT_TRUE(stream.good());
    EXPECT_EQ(stream.exceptions(), std::ios::badbit);  // the caller's setting is kept
    const BatchOutcome next = write_lines(stream, {"kept\n"});
    EXPECT_EQ(next.failed_lines, 0U);
    EXPECT_EQ(buffer.text(), "kept\n");
}

TEST(ResolveColorTest, AlwaysAndNeverAreObeyedAndAutoNeverColoursACallersStream) {
    const OutputStream callers(std::make_shared<std::ostringstream>());
    EXPECT_TRUE(resolve_color(ColorMode::Always, callers));
    EXPECT_FALSE(resolve_color(ColorMode::Never, callers));
    EXPECT_FALSE(resolve_color(ColorMode::Auto, callers));
    EXPECT_FALSE(callers.is_terminal());
    EXPECT_FALSE(callers.is_standard_error());
    EXPECT_FALSE(callers.terminal_size().has_value());
}

#ifndef PLATFORM_WINDOWS
TEST(ResolveColorTest, AutoColoursStandardErrorOnATerminalWithoutNoColorOrDumbTerm) {
    const test::ScopedEnvironment no_color("NO_COLOR", std::nullopt);
    const test::ScopedEnvironment term("TERM", "xterm");
    const test::StandardErrorOnPty pty(80, 24);
    const OutputStream standard_error(nullptr);
    EXPECT_TRUE(standard_error.is_terminal());
    EXPECT_TRUE(standard_error.is_standard_error());
    EXPECT_TRUE(resolve_color(ColorMode::Auto, standard_error));
}

TEST(ResolveColorTest, NoColorSetAndNotEmptyTurnsAutoColourOff) {
    const test::ScopedEnvironment no_color("NO_COLOR", "1");
    const test::ScopedEnvironment term("TERM", "xterm");
    const test::StandardErrorOnPty pty(80, 24);
    EXPECT_FALSE(resolve_color(ColorMode::Auto, OutputStream(nullptr)));
    EXPECT_TRUE(resolve_color(ColorMode::Always, OutputStream(nullptr)));
}

TEST(ResolveColorTest, AnEmptyNoColorDoesNotTurnAutoColourOff) {
    const test::ScopedEnvironment no_color("NO_COLOR", "");
    const test::ScopedEnvironment term("TERM", "xterm");
    const test::StandardErrorOnPty pty(80, 24);
    EXPECT_TRUE(resolve_color(ColorMode::Auto, OutputStream(nullptr)));
}

TEST(ResolveColorTest, DumbTermTurnsAutoColourOff) {
    const test::ScopedEnvironment no_color("NO_COLOR", std::nullopt);
    const test::ScopedEnvironment term("TERM", "dumb");
    const test::StandardErrorOnPty pty(80, 24);
    EXPECT_FALSE(resolve_color(ColorMode::Auto, OutputStream(nullptr)));
}

TEST(TerminalSizeTest, IsTheWindowSizeOfTheTerminalOnStandardError) {
    const test::StandardErrorOnPty pty(37, 9);
    const std::optional<TerminalSize> size = OutputStream(nullptr).terminal_size();
    ASSERT_TRUE(size.has_value());
    EXPECT_EQ(size.value_or(TerminalSize{}).columns, 37U);
    EXPECT_EQ(size.value_or(TerminalSize{}).rows, 9U);
}

TEST(TerminalSizeTest, IsUnknownWhenStandardErrorIsNotATerminal) {
    const test::StandardErrorToFile file;
    EXPECT_FALSE(OutputStream(nullptr).is_terminal());
    EXPECT_FALSE(OutputStream(nullptr).terminal_size().has_value());
    EXPECT_FALSE(resolve_color(ColorMode::Auto, OutputStream(nullptr)));
}
#endif

}  // namespace
}  // namespace lumen::detail
