// The pieces the line formats are built from (src/text_encoding.cpp): text
// escaping, JSON strings, logfmt values and numbers.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <string>
#include <string_view>

#include "text_encoding.hpp"

namespace lumen::detail {
namespace {

[[nodiscard]] std::string json(std::string_view text) {
    std::string out;
    append_json_string(out, text);
    return out;
}

[[nodiscard]] std::string logfmt(std::string_view text) {
    std::string out;
    append_logfmt_value(out, text);
    return out;
}

[[nodiscard]] std::string json_number(double value) {
    std::string out;
    append_json_number(out, value);
    return out;
}

// The text escape of a byte below 0x20, written out independently.
[[nodiscard]] std::string expected_text_escape(uint32_t byte) {
    switch (byte) {
        case '\n': return "\\n";
        case '\r': return "\\r";
        case '\t': return "\\t";
        default:   return std::format("\\x{:02x}", byte);
    }
}

// The JSON escape of a byte below 0x20, written out independently.
[[nodiscard]] std::string expected_json_escape(uint32_t byte) {
    switch (byte) {
        case '\b': return "\\b";
        case '\f': return "\\f";
        case '\n': return "\\n";
        case '\r': return "\\r";
        case '\t': return "\\t";
        default:   return std::format("\\u{:04x}", byte);
    }
}

TEST(TextEscapingTest, EveryC0ControlByteAndDelAreHexEscapesExceptNewlineReturnAndTab) {
    for (uint32_t byte = 0; byte < 0x20; ++byte) {
        const std::string input(1, static_cast<char>(byte));
        EXPECT_EQ(text_escaped(input), expected_text_escape(byte)) << byte;
    }
    EXPECT_EQ(text_escaped("\x7f"), "\\x7f");
}

TEST(TextEscapingTest, PrintableAsciiAndValidUtf8AreWrittenAsTheyAre) {
    EXPECT_EQ(text_escaped(" ~\"q\" \\ a=b"), " ~\"q\" \\ a=b");
    // e-acute, euro sign, an emoji, and U+00A0 (the first code point after C1).
    EXPECT_EQ(text_escaped("\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80\xc2\xa0"),
              "\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80\xc2\xa0");
}

TEST(TextEscapingTest, EveryC1ControlIsAUnicodeEscape) {
    for (uint32_t code_point = 0x80; code_point <= 0x9F; ++code_point) {
        // Two-byte UTF-8: 110000xx 10xxxxxx.
        const std::string input = {static_cast<char>(0xC0U | (code_point >> 6U)),
                                   static_cast<char>(0x80U | (code_point & 0x3FU))};
        EXPECT_EQ(text_escaped(input), std::format("\\u{:04x}", code_point)) << code_point;
    }
    EXPECT_EQ(text_escaped("a\xc2\x9b"
                           "2Jb"),
              "a\\u009b2Jb");  // CSI, which a terminal reads like ESC '['
}

TEST(TextEscapingTest, BidirectionalOverridesAndIsolatesAreUnicodeEscapes) {
    // U+202A..U+202E: E2 80 AA..AE. U+2066..U+2069: E2 81 A6..A9.
    for (uint32_t last = 0xAA; last <= 0xAE; ++last) {
        const std::string input = {'\xe2', '\x80', static_cast<char>(last)};
        EXPECT_EQ(text_escaped(input), std::format("\\u{:04x}", 0x2000U + last - 0x80U)) << last;
    }
    for (uint32_t last = 0xA6; last <= 0xA9; ++last) {
        const std::string input = {'\xe2', '\x81', static_cast<char>(last)};
        EXPECT_EQ(text_escaped(input), std::format("\\u{:04x}", 0x2040U + last - 0x80U)) << last;
    }
    // The neighbours are ordinary text.
    EXPECT_EQ(text_escaped("\xe2\x80\xa9\xe2\x80\xaf\xe2\x81\xa5\xe2\x81\xaa"),
              "\xe2\x80\xa9\xe2\x80\xaf\xe2\x81\xa5\xe2\x81\xaa");
}

TEST(TextEscapingTest, EachByteThatIsNotValidUtf8IsAHexEscape) {
    EXPECT_EQ(text_escaped("a\x80"
                           "b\xff"
                           "c\xc0\xaf"
                           "d\xed\xa0\x80"
                           "e\xc3"
                           "A"),
              "a\\x80b\\xffc\\xc0\\xafd\\xed\\xa0\\x80e\\xc3A");
}

TEST(TextEscapingTest, TruncatedSequenceAtTheEndOfTheViewIsNotCompletedFromTheBytesAfterIt) {
    // The view stops one byte before the end of a valid euro sign.
    const std::string storage = "x\xe2\x82\xac";
    EXPECT_EQ(text_escaped(std::string_view(storage.data(), 3)), "x\\xe2\\x82");
}

TEST(JsonStringTest, EscapesQuotesBackslashShortFormsAndEveryOtherC0Byte) {
    for (uint32_t byte = 0; byte < 0x20; ++byte) {
        const std::string input(1, static_cast<char>(byte));
        EXPECT_EQ(json(input), "\"" + expected_json_escape(byte) + "\"") << byte;
    }
    EXPECT_EQ(json("\"\\\x7f"), "\"\\\"\\\\\x7f\"");
}

TEST(JsonStringTest, LeadByteFollowedByANonContinuationByteBecomesOneReplacement) {
    EXPECT_EQ(json("x\xc3"
                   "A"),
              R"("x\ufffdA")");
    EXPECT_EQ(json("\xe2\x82"
                   "A"),
              R"("\ufffd\ufffdA")");
}

TEST(JsonStringTest, TruncatedSequenceAtTheEndOfTheViewBecomesOneReplacementPerByte) {
    const std::string storage = "x\xe2\x82\xac";
    EXPECT_EQ(json(std::string_view(storage.data(), 3)), R"("x\ufffd\ufffd")");
    EXPECT_EQ(json(storage), "\"x\xe2\x82\xac\"");
}

TEST(JsonStringTest, OverlongFormsSurrogatesAndCodePointsAboveTheRangeAreReplaced) {
    EXPECT_EQ(json("\xc0\xaf"), R"("\ufffd\ufffd")");                      // overlong '/'
    EXPECT_EQ(json("\xed\xa0\x80"), R"("\ufffd\ufffd\ufffd")");            // U+D800
    EXPECT_EQ(json("\xf4\x90\x80\x80"), R"("\ufffd\ufffd\ufffd\ufffd")");  // U+110000
    EXPECT_EQ(json("\xf4\x8f\xbf\xbf"), "\"\xf4\x8f\xbf\xbf\"");           // U+10FFFF is valid
}

TEST(JsonStringTest, AReplacementCharacterInTheTextIsWrittenLikeAReplacedByte) {
    EXPECT_EQ(json("\xef\xbf\xbd"), json("\xff"));
    EXPECT_EQ(json("a\xef\xbf\xbd"), R"("a\ufffd")");
}

TEST(LogfmtValueTest, IsQuotedWhenEmptyOrHoldingASpaceEqualsQuoteBackslashOrControl) {
    EXPECT_EQ(logfmt("plain"), "plain");
    EXPECT_EQ(logfmt(""), R"("")");
    EXPECT_EQ(logfmt("a b"), R"("a b")");
    EXPECT_EQ(logfmt("a=b"), R"("a=b")");
    EXPECT_EQ(logfmt("a\"b"), R"("a\"b")");
    EXPECT_EQ(logfmt("a\\b"), R"("a\\b")");
    EXPECT_EQ(logfmt("a\x01"), R"("a\u0001")");
    EXPECT_EQ(logfmt("a\x7f"), "\"a\x7f\"");
}

TEST(JsonNumberTest, NegativeZeroKeepsItsSignAsMinusZeroPointZero) {
    EXPECT_EQ(json_number(-0.0), "-0.0");
    EXPECT_EQ(json_number(0.0), "0");
    EXPECT_EQ(json_number(1.0), "1");
    EXPECT_EQ(json_number(0.25), "0.25");
}

TEST(DoubleTextTest, NonFiniteValuesHaveTheirNames) {
    std::string out;
    append_double(out, std::numeric_limits<double>::quiet_NaN());
    out += ' ';
    append_double(out, std::numeric_limits<double>::infinity());
    out += ' ';
    append_double(out, -std::numeric_limits<double>::infinity());
    EXPECT_EQ(out, "NaN Infinity -Infinity");
    EXPECT_FALSE(has_json_number(std::nan("")));
    EXPECT_TRUE(has_json_number(-0.0));
}

TEST(LevelNameTest, EachLevelHasItsNameAndAnythingElseIsUnknown) {
    EXPECT_EQ(level_name(LogLevel::Trace), "TRACE");
    EXPECT_EQ(level_name(LogLevel::Debug), "DEBUG");
    EXPECT_EQ(level_name(LogLevel::Info), "INFO");
    EXPECT_EQ(level_name(LogLevel::Warn), "WARN");
    EXPECT_EQ(level_name(LogLevel::Error), "ERROR");
    EXPECT_EQ(level_name(LogLevel::Fatal), "FATAL");
    EXPECT_EQ(level_name(static_cast<LogLevel>(6)), "UNKNOWN");
}

}  // namespace
}  // namespace lumen::detail
