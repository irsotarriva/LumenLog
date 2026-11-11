// TimeFormat (src/time_format.cpp): the patterns it accepts and refuses, the
// %L and %N fields, and UTC against local time.
#include <gtest/gtest.h>

#include <cstdint>
#include <ctime>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include "lumen/error.hpp"
#include "sink_test_support.hpp"
#include "time_format.hpp"

namespace lumen::detail {
namespace {

using test::fixed_time_ns;  // 2026-11-11T10:00:00.123456789Z

constexpr uint64_t whole_second_ns = fixed_time_ns - 123'456'789;

[[nodiscard]] std::string formatted(std::string_view pattern, uint64_t timestamp_ns,
                                    TimeZone zone = TimeZone::Utc) {
    const std::expected<TimeFormat, std::error_code> format = TimeFormat::parse(pattern, zone);
    EXPECT_TRUE(format.has_value()) << pattern;
    if (!format) {
        return {};
    }
    std::string out;
    format->append(out, timestamp_ns);
    return out;
}

[[nodiscard]] std::optional<std::error_code> refusal(std::string_view pattern) {
    const std::expected<TimeFormat, std::error_code> format =
        TimeFormat::parse(pattern, TimeZone::Utc);
    return format ? std::nullopt : std::optional<std::error_code>(format.error());
}

TEST(TimeFormatTest, MillisecondsArePaddedToThreeDigits) {
    EXPECT_EQ(formatted("%L", whole_second_ns + 7'000'000), "007");
    EXPECT_EQ(formatted("%L", whole_second_ns), "000");
    EXPECT_EQ(formatted("%L", whole_second_ns + 999'999'999), "999");
}

TEST(TimeFormatTest, NanosecondsArePaddedToNineDigits) {
    EXPECT_EQ(formatted("%N", whole_second_ns + 5), "000000005");
    EXPECT_EQ(formatted("%N", whole_second_ns + 7'000'000), "007000000");
    EXPECT_EQ(formatted("%N", fixed_time_ns), "123456789");
}

TEST(TimeFormatTest, StrftimeConversionsAndLiteralTextMixWithTheSubsecondFields) {
    EXPECT_EQ(formatted("[%Y-%m-%d %H:%M:%S.%N|%L%%]", fixed_time_ns),
              "[2026-11-11 10:00:00.123456789|123%]");
    EXPECT_EQ(formatted("%%L %%N", fixed_time_ns), "%L %N");
    EXPECT_EQ(formatted("%%%N", fixed_time_ns), "%123456789");
}

TEST(TimeFormatTest, AnEmptyPatternIsEmptyAndWritesNothing) {
    const std::expected<TimeFormat, std::error_code> format = TimeFormat::parse("", TimeZone::Utc);
    ASSERT_TRUE(format.has_value());
    EXPECT_TRUE(format->empty());
    EXPECT_EQ(formatted("", fixed_time_ns), "");
}

TEST(TimeFormatTest, ALiteralRunLongerThanTheShortBufferIsWrittenWhole) {
    const std::string literal(1000, 'x');
    EXPECT_EQ(formatted(literal + "%H" + literal, fixed_time_ns), literal + "10" + literal);
}

TEST(TimeFormatTest, NewlineTabAndUnknownConversionsAreRefused) {
    for (const std::string_view pattern : {"%n", "%H%t%M", "%", "%H:%", "%Q", "%Ec", "%Oy", "%K"}) {
        const std::optional<std::error_code> error = refusal(pattern);
        ASSERT_TRUE(error.has_value()) << pattern;
        EXPECT_EQ(error.value_or(std::error_code{}), LumenError::InvalidSinkConfig) << pattern;
    }
}

TEST(TimeFormatTest, ANulCharacterAnywhereInThePatternIsRefused) {
    using namespace std::string_view_literals;
    for (const std::string_view pattern : {"a\0b%Y"sv, "%Y\0"sv, "\0"sv}) {
        const std::optional<std::error_code> error = refusal(pattern);
        ASSERT_TRUE(error.has_value()) << pattern.size();
        EXPECT_EQ(error.value_or(std::error_code{}), LumenError::InvalidSinkConfig);
    }
}

#ifdef PLATFORM_WINDOWS
TEST(TimeFormatTest, UtcAndLocalTimeDifferByTheZonesOffset) {
    GTEST_SKIP() << "TZ with an IANA zone name needs a POSIX C library";
}
#else
TEST(TimeFormatTest, UtcAndLocalTimeDifferByTheZonesOffset) {
    const test::ScopedTimeZone india("Asia/Kolkata");  // UTC+05:30, no daylight saving time
    EXPECT_EQ(formatted("%H:%M:%S", fixed_time_ns, TimeZone::Utc), "10:00:00");
    EXPECT_EQ(formatted("%H:%M:%S", fixed_time_ns, TimeZone::Local), "15:30:00");
}
#endif

TEST(UtcDateTest, IsTheUtcCalendarDateOfTheTimestamp) {
    EXPECT_EQ(utc_date(fixed_time_ns), "2026-11-11");
    EXPECT_EQ(utc_date(0), "1970-01-01");
    EXPECT_EQ(utc_date(whole_second_ns + uint64_t{14} * 3600 * 1'000'000'000), "2026-11-12");
}

}  // namespace
}  // namespace lumen::detail
