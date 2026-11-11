// RecordBuilder and MetricBuilder: messages, tag values and tag rules.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <utility>
#include <vector>

#include "lumen/limits.hpp"
#include "lumen/record_builder.hpp"
#include "test_support.hpp"

namespace lumen {
namespace {

using test::LocalCoreTest;
using test::log_formatted_to;
using test::log_to;
using test::metric_to;
using test::tag_of;
using test::tags_of;

class RecordBuilderTest : public LocalCoreTest {
protected:
    [[nodiscard]] LogRecord _only_log() {
        _core.flush();
        const auto logs = _captured->logs();
        EXPECT_EQ(logs.size(), 1U);
        return logs.empty() ? LogRecord{} : logs[0];
    }
};

TEST_F(RecordBuilderTest, SingleArgumentMessageIsLoggedVerbatimIncludingBraces) {
    log_to(_core, "{} stays {}");
    EXPECT_EQ(_only_log().message, "{} stays {}");
}

TEST_F(RecordBuilderTest, FormatArgumentsAreFormattedWithStdFormat) {
    log_formatted_to(_core, "{} + {} = {:.1f}", 1, 2, 3.0);
    EXPECT_EQ(_only_log().message, "1 + 2 = 3.0");
}

TEST_F(RecordBuilderTest, FormattedMessageLongerThanTheInlineScratchBufferIsComplete) {
    const std::string long_part(2000, 'x');
    log_formatted_to(_core, "[{}]", long_part);
    EXPECT_EQ(_only_log().message, "[" + long_part + "]");
}

TEST_F(RecordBuilderTest, ManyLongTagsOverflowingTheScratchBufferAllArriveIntact) {
    {
        auto builder = log_to(_core, "long tags");
        for (int i = 0; i < 8; ++i) {
            builder.tag("key" + std::to_string(i), std::string(300, static_cast<char>('a' + i)));
        }
    }
    const LogRecord r = _only_log();
    ASSERT_EQ(r.tags.count(), 8U);
    for (int i = 0; i < 8; ++i) {
        EXPECT_EQ(tag_of(r.tags, "key" + std::to_string(i)),
                  std::string(300, static_cast<char>('a' + i)));
    }
}

TEST_F(RecordBuilderTest, TaggingAKeyTwiceKeepsOneEntryWithTheLaterValue) {
    log_to(_core, "dup").tag("k", "a").tag("other", "x").tag("k", "b");
    EXPECT_EQ(tags_of(_only_log().tags),
              (std::vector<std::pair<std::string, std::string>>{{"k", "b"}, {"other", "x"}}));
}

TEST_F(RecordBuilderTest, TagPastTheRecordLimitIsLeftOutAndCounted) {
    {
        auto builder = log_to(_core, "full");
        for (std::size_t i = 0; i < max_record_tags + 3; ++i) {
            builder.tag("k" + std::to_string(i), i);
        }
        builder.tag("k0", "replaced on a full record");
    }
    const LogRecord r = _only_log();
    EXPECT_EQ(r.tags.count(), max_record_tags);
    EXPECT_EQ(tag_of(r.tags, "k0"), "replaced on a full record");
    EXPECT_EQ(tag_of(r.tags, "k" + std::to_string(max_record_tags - 1)),
              std::to_string(max_record_tags - 1));
    EXPECT_EQ(tag_of(r.tags, "k" + std::to_string(max_record_tags)), std::nullopt);
    EXPECT_EQ(_core.statistics().tags_dropped, 3U);
}

TEST_F(RecordBuilderTest, IntegerTagsAreWrittenExactlyAtTheirExtremes) {
    log_to(_core, "ints")
        .tag("i64_max", std::numeric_limits<int64_t>::max())
        .tag("i64_min", std::numeric_limits<int64_t>::min())
        .tag("u64_max", std::numeric_limits<uint64_t>::max())
        .tag("negative", -42)
        .tag("u8", uint8_t{200});
    const LogRecord r = _only_log();
    EXPECT_EQ(tag_of(r.tags, "i64_max"), "9223372036854775807");
    EXPECT_EQ(tag_of(r.tags, "i64_min"), "-9223372036854775808");
    EXPECT_EQ(tag_of(r.tags, "u64_max"), "18446744073709551615");
    EXPECT_EQ(tag_of(r.tags, "negative"), "-42");
    EXPECT_EQ(tag_of(r.tags, "u8"), "200");
}

TEST_F(RecordBuilderTest, FloatingPointTagsReadBackAsTheSameValue) {
    const double tiny = 1e-300;
    const double awkward = 0.1 + 0.2;
    log_to(_core, "doubles").tag("tiny", tiny).tag("awkward", awkward).tag("float", 1.5F);
    const LogRecord r = _only_log();
    const std::optional<std::string> tiny_text = tag_of(r.tags, "tiny");
    const std::optional<std::string> awkward_text = tag_of(r.tags, "awkward");
    ASSERT_TRUE(tiny_text.has_value() && awkward_text.has_value());
    EXPECT_EQ(std::stod(tiny_text.value_or("")), tiny);
    EXPECT_EQ(std::stod(awkward_text.value_or("")), awkward);
    EXPECT_EQ(tag_of(r.tags, "float"), "1.5");
}

TEST_F(RecordBuilderTest, BoolTagsAreTrueOrFalseAndStringLiteralsStayStrings) {
    log_to(_core, "bools").tag("yes", true).tag("no", false).tag("literal", "text");
    const LogRecord r = _only_log();
    EXPECT_EQ(tag_of(r.tags, "yes"), "true");
    EXPECT_EQ(tag_of(r.tags, "no"), "false");
    EXPECT_EQ(tag_of(r.tags, "literal"), "text");
}

TEST_F(RecordBuilderTest, TagFromATemporaryStringSurvivesUntilDelivery) {
    log_to(_core, "temporary").tag(std::string("key_") + "built", std::string(100, 'v'));
    EXPECT_EQ(tag_of(_only_log().tags, "key_built"), std::string(100, 'v'));
}

TEST_F(RecordBuilderTest, ClassNameIsTakenFromTheEnclosingMemberFunction) {
    struct Emitter {
        static void emit(Core& core) {
            const RecordBuilder statement(core, LogLevel::Info, std::source_location::current(),
                                          "member");
        }
    };
    Emitter::emit(_core);
    EXPECT_EQ(_only_log().source.class_name, "Emitter");
}

TEST_F(RecordBuilderTest, SourceGivenAsTextIsCopiedAndOutlivesTheStringsItCameFrom) {
    {
        auto file = std::make_unique<std::string>("/scripts/" + std::string(600, 'f') + ".py");
        auto function = std::make_unique<std::string>("Trainer.step_" + std::string(40, 'x'));
        auto class_name = std::make_unique<std::string>("Trainer" + std::string(40, 'c'));
        {
            const RecordBuilder statement(_core, LogLevel::Warn,
                                          SourceLocation{*file, *function, *class_name, 17},
                                          "from text");
        }
        // Freed before delivery: ASan reports a record still pointing at them.
        file.reset();
        function.reset();
        class_name.reset();
    }
    const LogRecord r = _only_log();
    EXPECT_EQ(r.level, LogLevel::Warn);
    EXPECT_EQ(r.message, "from text");
    EXPECT_EQ(r.source.file, "/scripts/" + std::string(600, 'f') + ".py");
    EXPECT_EQ(r.source.function, "Trainer.step_" + std::string(40, 'x'));
    EXPECT_EQ(r.source.class_name, "Trainer" + std::string(40, 'c'));
    EXPECT_EQ(r.source.line, 17U);
}

TEST_F(RecordBuilderTest, MetricBuilderAppliesTheSameTagRules) {
    metric_to(_core, "loss", 0.25).tag("epoch", 3).tag("epoch", 4).tag("ok", true);
    _core.flush();
    const auto metrics = _captured->metrics();
    ASSERT_EQ(metrics.size(), 1U);
    EXPECT_EQ(metrics[0].name, "loss");
    EXPECT_DOUBLE_EQ(metrics[0].value, 0.25);
    EXPECT_EQ(tags_of(metrics[0].tags),
              (std::vector<std::pair<std::string, std::string>>{{"epoch", "4"}, {"ok", "true"}}));
}

}  // namespace
}  // namespace lumen
