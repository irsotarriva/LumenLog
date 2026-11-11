// The exact lines of the three FileFormats (src/record_format.cpp), read back
// from a FileSink's file.
#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "lumen/file_sink.hpp"
#include "sink_test_support.hpp"

namespace lumen {
namespace {

using test::fixed_time_iso;
using test::log_record;
using test::metric_record;
using test::progress_record;

class RecordFormatTest : public ::testing::Test {
protected:
    // Writes the records straight into a new FileSink of `format` and returns
    // the file's lines.
    template <test::AnyRecord... Records>
    [[nodiscard]] std::vector<std::string> _lines(FileFormat format, const Records&... records) {
        const auto path = dir_ / ("out" + std::to_string(++files_) + ".log");
        auto sink = FileSink::create({.path = path, .format = format, .diagnostics = {}});
        EXPECT_TRUE(sink.has_value());
        if (!sink) {
            return {};
        }
        (test::deliver(**sink, records), ...);
        (*sink)->flush();
        return test::read_lines(path);
    }

    // The single line written for `record`.
    template <test::AnyRecord Record>
    [[nodiscard]] std::string _line(FileFormat format, const Record& record) {
        const std::vector<std::string> lines = _lines(format, record);
        EXPECT_EQ(lines.size(), 1U);
        return lines.empty() ? std::string{} : lines.front();
    }

    // The text after `"value":` in a Json metric line, up to the next ','.
    [[nodiscard]] std::string _json_value_text(double value) {
        const std::string line = _line(FileFormat::Json, metric_record("m", value));
        const std::string key = R"("value":)";
        const std::size_t start = line.find(key);
        EXPECT_NE(start, std::string::npos) << line;
        if (start == std::string::npos) {
            return {};
        }
        const std::size_t begin = start + key.size();
        return line.substr(begin, line.find(',', begin) - begin);
    }

private:
    test::TempDir dir_;
    uint32_t files_ = 0;
};

[[nodiscard]] LogRecord tagged_log() {
    LogRecord record = log_record("connected");
    EXPECT_TRUE(record.tags.add("host", "db1"));
    return record;
}

TEST_F(RecordFormatTest, TextLinesStartWithTimeAndKindAndEndWithTagsThreadAndSource) {
    const std::string time(fixed_time_iso);
    EXPECT_EQ(_lines(FileFormat::Text, tagged_log(), metric_record("loss", 0.25),
                     progress_record("files", 3, 10)),
              (std::vector<std::string>{
                  time + " INFO  connected host=db1 [t3] (main.cpp:42)",
                  time + " METRIC loss=0.25 [t3]",
                  time + " PROGRESS files 3/10 [t3]",
              }));
}

TEST_F(RecordFormatTest, TextEscapesControlBytesAndLeavesEverythingElse) {
    EXPECT_EQ(_line(FileFormat::Text, log_record("a\nb\tc\r\x1b[31m\x7f \"q\" \\ \xc3\xa9")),
              std::string(fixed_time_iso) +
                  " INFO  a\\nb\\tc\\r\\x1b[31m\\x7f \"q\" \\ \xc3\xa9 [t3] (main.cpp:42)");
}

TEST_F(RecordFormatTest, KeyValueLinesHaveFixedKeysThenThreadThenPrefixedTags) {
    const std::string time = "time=" + std::string(fixed_time_iso);
    EXPECT_EQ(_lines(FileFormat::KeyValue, tagged_log(), metric_record("loss", 0.25),
                     progress_record("files", 3, 10)),
              (std::vector<std::string>{
                  time + " kind=log level=INFO msg=connected file=main.cpp line=42 "
                         "function=run thread=3 tags.host=db1",
                  time + " kind=metric name=loss value=0.25 thread=3",
                  time + " kind=progress id=7 label=files current=3 total=10 thread=3",
              }));
}

TEST_F(RecordFormatTest, KeyValueQuotesEmptyValuesAndValuesWithSpacesEqualsQuotesOrControls) {
    LogRecord record = log_record("two words");
    record.source.class_name = "Server";
    EXPECT_TRUE(record.tags.add("empty", ""));
    EXPECT_TRUE(record.tags.add("eq", "a=b"));
    EXPECT_TRUE(record.tags.add("quote", "say \"hi\""));
    EXPECT_TRUE(record.tags.add("nl", "x\ny"));
    EXPECT_TRUE(record.tags.add("my key", "plain"));
    EXPECT_EQ(_line(FileFormat::KeyValue, record),
              "time=" + std::string(fixed_time_iso) +
                  R"( kind=log level=INFO msg="two words" file=main.cpp line=42 function=run )"
                  R"(class=Server thread=3 tags.empty="" tags.eq="a=b" )"
                  R"(tags.quote="say \"hi\"" tags.nl="x\ny" "tags.my key"=plain)");
}

TEST_F(RecordFormatTest, JsonLinesForEachRecordKind) {
    EXPECT_EQ(_lines(FileFormat::Json, tagged_log(), metric_record("loss", 0.25),
                     progress_record("files", 3, 10)),
              (std::vector<std::string>{
                  R"({"type":"log","ts":1794391200123456789,"level":"INFO","msg":"connected",)"
                  R"("file":"main.cpp","line":42,"function":"run","class":"","thread":3,)"
                  R"("tags":{"host":"db1"}})",
                  R"({"type":"metric","ts":1794391200123456789,"name":"loss","value":0.25,)"
                  R"("thread":3,"tags":{}})",
                  R"({"type":"progress","ts":1794391200123456789,"id":7,"label":"files",)"
                  R"("current":3,"total":10,"thread":3,"tags":{}})",
              }));
}

TEST_F(RecordFormatTest, JsonLevelIsThePlainNameAndUnknownOutsideTheEnum) {
    EXPECT_NE(_line(FileFormat::Json, log_record("w", LogLevel::Warn)).find(R"("level":"WARN",)"),
              std::string::npos);
    EXPECT_NE(_line(FileFormat::Json, log_record("x", static_cast<LogLevel>(9)))
                  .find(R"("level":"UNKNOWN",)"),
              std::string::npos);
}

TEST_F(RecordFormatTest, JsonMetricValueIsTheShortestTextThatReadsBackBitForBit) {
    EXPECT_EQ(_json_value_text(1e-9), "1e-09");
    EXPECT_EQ(_json_value_text(123456789.123456789), "123456789.12345679");
    EXPECT_EQ(_json_value_text(-0.0), "-0");
    const std::vector<double> values = {
        0.1,
        1e-9,
        123456789.123456789,
        std::numeric_limits<double>::denorm_min(),
        std::numeric_limits<double>::min(),
        std::numeric_limits<double>::max(),
        -std::numeric_limits<double>::max(),
        1.0 / 3.0,
    };
    for (const double value : values) {
        const std::string text = _json_value_text(value);
        // strtod, not std::from_chars: libc++ has no floating-point from_chars.
        char* end = nullptr;
        const double parsed = std::strtod(text.c_str(), &end);
        EXPECT_EQ(end, text.c_str() + text.size()) << text;
        EXPECT_EQ(std::bit_cast<uint64_t>(parsed), std::bit_cast<uint64_t>(value)) << text;
    }
}

TEST_F(RecordFormatTest, JsonNonFiniteValueIsNullWithItsNameInValueText) {
    const auto line_for = [&](double value) {
        return _line(FileFormat::Json, metric_record("m", value));
    };
    EXPECT_NE(line_for(std::numeric_limits<double>::quiet_NaN())
                  .find(R"("value":null,"value_text":"NaN",)"),
              std::string::npos);
    EXPECT_NE(line_for(std::numeric_limits<double>::infinity())
                  .find(R"("value":null,"value_text":"Infinity",)"),
              std::string::npos);
    EXPECT_NE(line_for(-std::numeric_limits<double>::infinity())
                  .find(R"("value":null,"value_text":"-Infinity",)"),
              std::string::npos);
    EXPECT_EQ(line_for(1.5).find("value_text"), std::string::npos);
}

TEST_F(RecordFormatTest, JsonEscapesQuotesBackslashesAndEveryControlByte) {
    const std::string message = std::string("\"\\\b\f\n\r\t") + '\0' + "\x01\x1f\x7f";
    const LogRecord record = log_record(message);
    EXPECT_NE(_line(FileFormat::Json, record)
                  .find(R"("msg":"\"\\\b\f\n\r\t\u0000\u0001\u001f)"
                        "\x7f\""),
              std::string::npos);
}

TEST_F(RecordFormatTest, JsonKeepsValidUtf8AndReplacesEachInvalidByte) {
    // e-acute, euro sign, an emoji: valid, written as they are.
    EXPECT_NE(_line(FileFormat::Json, log_record("\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80"))
                  .find("\"msg\":\"\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80\""),
              std::string::npos);
    // A stray continuation byte, 0xFF, an overlong '/', a surrogate, a
    // truncated sequence at the end.
    EXPECT_NE(_line(FileFormat::Json, log_record("a\x80"
                                                 "b\xff"
                                                 "c\xc0\xaf"
                                                 "d\xed\xa0\x80"
                                                 "e\xe2\x82"))
                  .find(R"("msg":"a\ufffdb\ufffdc\ufffd\ufffdd\ufffd\ufffd\ufffde\ufffd\ufffd")"),
              std::string::npos);
}

TEST_F(RecordFormatTest, JsonTagsNamedLikeRecordKeysStayInsideTagsAndKeysAppearOnce) {
    LogRecord record = log_record("m");
    for (const char* key : {"type", "ts", "msg", "level", "file", "line", "thread", "tags"}) {
        EXPECT_TRUE(record.tags.add(key, "x"));
    }
    const std::string line = _line(FileFormat::Json, record);
    EXPECT_NE(line.find(R"("tags":{"type":"x","ts":"x","msg":"x","level":"x","file":"x",)"
                        R"("line":"x","thread":"x","tags":"x"}})"),
              std::string::npos)
        << line;
    for (const char* key : {R"("type":)", R"("ts":)", R"("msg":)", R"("level":)"}) {
        const std::size_t first = line.find(key);
        const std::size_t second = line.find(key, first + 1);
        const std::size_t tags = line.find(R"("tags":{)");
        EXPECT_LT(first, tags) << key;
        EXPECT_GT(second, tags) << key;  // the second one is the tag
        EXPECT_EQ(line.find(key, second + 1), std::string::npos) << key;
    }
}

TEST_F(RecordFormatTest, TagValuesAreJsonStringsWhateverTheyLookLike) {
    MetricRecord record = metric_record("m", 1.0);
    EXPECT_TRUE(record.tags.add("count", "3"));
    EXPECT_TRUE(record.tags.add("flag", "true"));
    EXPECT_NE(_line(FileFormat::Json, record).find(R"("tags":{"count":"3","flag":"true"})"),
              std::string::npos);
}

TEST_F(RecordFormatTest, KeyRepeatedInAHandBuiltTagSetIsWrittenOnceWithTheFirstValue) {
    LogRecord record = log_record("m");
    EXPECT_TRUE(record.tags.add("k", "first"));
    EXPECT_TRUE(record.tags.add("k", "second"));
    EXPECT_NE(_line(FileFormat::Json, record).find(R"("tags":{"k":"first"})"), std::string::npos);
    EXPECT_NE(_line(FileFormat::KeyValue, record).find(" tags.k=first"), std::string::npos);
    EXPECT_EQ(_line(FileFormat::KeyValue, record).find("second"), std::string::npos);
    EXPECT_EQ(_line(FileFormat::Text, record).find("second"), std::string::npos);
}

TEST_F(RecordFormatTest, EmptyStringsAreWrittenAsEmptyJsonStringsAndQuotedLogfmtValues) {
    LogRecord record = log_record("");
    record.source = {};
    EXPECT_TRUE(record.tags.add("", ""));
    EXPECT_NE(_line(FileFormat::Json, record)
                  .find(R"("msg":"","file":"","line":0,"function":"","class":"","thread":3,)"
                        R"("tags":{"":""}})"),
              std::string::npos);
    EXPECT_NE(
        _line(FileFormat::KeyValue, record).find(R"(msg="" file="" line=0 thread=3 tags.="")"),
        std::string::npos);
}

}  // namespace
}  // namespace lumen
