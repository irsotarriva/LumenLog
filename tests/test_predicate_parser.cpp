#include <clocale>
#include <initializer_list>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "lumen/error.hpp"
#include "lumen/predicate.hpp"

namespace lumen {
namespace {

TagSet<16> tags_of(std::initializer_list<std::pair<std::string_view, std::string_view>> kv) {
    TagSet<16> tags;
    for (const auto& [k, v] : kv) EXPECT_TRUE(tags.add(k, v));
    return tags;
}

// Parses `query` (failing the test on a parse error) and evaluates it.
bool eval(std::string_view query, const TagSet<16>& tags, LogLevel level = LogLevel::Info) {
    const auto parsed = parse_predicate(query);
    EXPECT_TRUE(parsed.has_value()) << "query: " << query << "\n  error at "
                                    << (parsed ? 0 : parsed.error().position) << ": "
                                    << (parsed ? "" : parsed.error().message);
    return parsed.has_value() && parsed->evaluate(tags, level);
}

PredicateParseError parse_error(std::string_view query) {
    auto parsed = parse_predicate(query);
    EXPECT_FALSE(parsed.has_value()) << "query unexpectedly parsed: " << query;
    return parsed ? PredicateParseError{} : parsed.error();
}

// ── The examples in predicate.hpp ────────────────────────────────────────────

TEST(PredicateParserTest, FirstExampleQueryFromHeader) {
    constexpr std::string_view query = "level >= WARN && run_id == 42 && !exists(suppress)";
    const auto run = tags_of({{"run_id", "42"}});
    const auto suppressed = tags_of({{"run_id", "42"}, {"suppress", "1"}});
    const auto other = tags_of({{"run_id", "7"}});

    EXPECT_TRUE(eval(query, run, LogLevel::Warn));
    EXPECT_TRUE(eval(query, run, LogLevel::Fatal));
    EXPECT_FALSE(eval(query, run, LogLevel::Info));
    EXPECT_FALSE(eval(query, suppressed, LogLevel::Error));
    EXPECT_FALSE(eval(query, other, LogLevel::Error));
}

TEST(PredicateParserTest, SecondExampleQueryFromHeader) {
    constexpr std::string_view query =
        R"((stage == "warmup" || stage == steady) && temperature < 350)";
    EXPECT_TRUE(eval(query, tags_of({{"stage", "warmup"}, {"temperature", "300"}})));
    EXPECT_TRUE(eval(query, tags_of({{"stage", "steady"}, {"temperature", "349.9"}})));
    EXPECT_FALSE(eval(query, tags_of({{"stage", "steady"}, {"temperature", "350"}})));
    EXPECT_FALSE(eval(query, tags_of({{"stage", "cooldown"}, {"temperature", "300"}})));
}

// ── Level comparisons ────────────────────────────────────────────────────────

TEST(PredicateParserTest, LevelOperators) {
    const TagSet<16> none;
    EXPECT_TRUE(eval("level == INFO", none, LogLevel::Info));
    EXPECT_FALSE(eval("level != INFO", none, LogLevel::Info));
    EXPECT_TRUE(eval("level < WARN", none, LogLevel::Info));
    EXPECT_FALSE(eval("level <= DEBUG", none, LogLevel::Info));
    EXPECT_TRUE(eval("level > DEBUG", none, LogLevel::Info));
    EXPECT_TRUE(eval("level >= INFO", none, LogLevel::Info));
}

TEST(PredicateParserTest, LevelNamesAreCaseInsensitiveAndNumericLevelsWork) {
    const TagSet<16> none;
    EXPECT_TRUE(eval("level >= warn", none, LogLevel::Error));
    EXPECT_TRUE(eval("LEVEL == Error", none, LogLevel::Error));
    EXPECT_TRUE(eval("level == 4", none, LogLevel::Error));
}

// ── Tag comparisons ──────────────────────────────────────────────────────────

TEST(PredicateParserTest, NumericEqualityIgnoresSpelling) {
    EXPECT_TRUE(eval("sensor_id == 42", tags_of({{"sensor_id", "42.0"}})));
    EXPECT_TRUE(eval("gain == 1e3", tags_of({{"gain", "1000"}})));
    EXPECT_FALSE(eval("sensor_id == 42", tags_of({{"sensor_id", "42abc"}})));
}

TEST(PredicateParserTest, NumericOrdering) {
    const auto tags = tags_of({{"pressure", "69999.97"}, {"temp", "-12.5"}});
    EXPECT_TRUE(eval("pressure < 70000", tags));
    EXPECT_FALSE(eval("pressure >= 70000", tags));
    EXPECT_TRUE(eval("temp < -10", tags));
    EXPECT_TRUE(eval("temp > -1e2", tags));
    EXPECT_TRUE(eval("pressure <= +69999.97", tags));
}

TEST(PredicateParserTest, StringComparisons) {
    const auto tags = tags_of({{"stage", "warmup"}, {"name", "pump A"}});
    EXPECT_TRUE(eval("stage == warmup", tags));
    EXPECT_TRUE(eval("stage == \"warmup\"", tags));
    EXPECT_TRUE(eval("name == 'pump A'", tags));
    EXPECT_TRUE(eval("stage != steady", tags));
    EXPECT_FALSE(eval("stage != warmup", tags));
}

TEST(PredicateParserTest, EscapedQuotesInStrings) {
    EXPECT_TRUE(eval(R"(msg == "say \"hi\"")", tags_of({{"msg", "say \"hi\""}})));
}

TEST(PredicateParserTest, ComparisonsAgainstMissingTagAreFalse) {
    const TagSet<16> none;
    EXPECT_FALSE(eval("pressure < 70000", none));
    EXPECT_FALSE(eval("pressure != 5", none));
    EXPECT_FALSE(eval("stage != steady", none));
    EXPECT_TRUE(eval("!exists(stage) || stage != steady", none));
}

TEST(PredicateParserTest, EmptyStringComparisonsAgainstMissingTagAreFalse) {
    const TagSet<16> none;
    EXPECT_FALSE(eval(R"(k == "")", none));
    EXPECT_FALSE(eval("k == ''", none));
    EXPECT_FALSE(eval(R"(k != "")", none));
    EXPECT_FALSE(eval(R"(k != "x")", none));
    EXPECT_TRUE(eval(R"(k == "")", tags_of({{"k", ""}})));
    EXPECT_FALSE(eval(R"(k != "")", tags_of({{"k", ""}})));
}

TEST(PredicateParserTest, EveryOperatorAgainstMissingTagIsFalse) {
    const TagSet<16> none;
    for (const std::string_view op : {"==", "!=", "<", "<=", ">", ">="}) {
        const std::string query = "k " + std::string(op) + " 0";
        EXPECT_FALSE(eval(query, none)) << query;
        const auto parsed = parse_predicate(query);
        ASSERT_TRUE(parsed.has_value()) << query;
        EXPECT_FALSE(parsed->evaluate(none)) << query << " (record without a level)";
    }
}

TEST(PredicateParserTest, FirstOfDuplicateKeysDecides) {
    const auto text = tags_of({{"k", "a"}, {"k", "b"}});
    EXPECT_TRUE(eval("k == a", text));
    EXPECT_FALSE(eval("k == b", text));
    EXPECT_TRUE(eval("k != b", text));
    EXPECT_FALSE(eval("k != a", text));

    const auto numbers = tags_of({{"n", "1"}, {"n", "5"}});
    EXPECT_TRUE(eval("n == 1", numbers));
    EXPECT_FALSE(eval("n > 3", numbers));
}

TEST(PredicateParserTest, ExistsMatchesTagWithEmptyValue) {
    EXPECT_TRUE(eval("exists(flag)", tags_of({{"flag", ""}})));
    EXPECT_FALSE(eval("exists(flag)", tags_of({{"other", "x"}})));
}

TEST(PredicateParserTest, QuotedKeyReachesTagNamedLikeKeyword) {
    EXPECT_TRUE(eval("\"level\" == 3", tags_of({{"level", "3"}}), LogLevel::Trace));
    EXPECT_TRUE(eval("exists(\"and\")", tags_of({{"and", "x"}})));
}

TEST(PredicateParserTest, DottedKeys) {
    EXPECT_TRUE(eval("sensor.id == 42", tags_of({{"sensor.id", "42"}})));
}

// ── Boolean structure ────────────────────────────────────────────────────────

TEST(PredicateParserTest, AndBindsTighterThanOr) {
    // true || (false && false)  ==  true
    EXPECT_TRUE(eval("a == 1 || b == 1 && c == 1", tags_of({{"a", "1"}})));
    // (true || false) && false  ==  false
    EXPECT_FALSE(eval("(a == 1 || b == 1) && c == 1", tags_of({{"a", "1"}})));
}

TEST(PredicateParserTest, WordOperatorsAndLiterals) {
    const auto tags = tags_of({{"a", "1"}});
    EXPECT_TRUE(eval("a == 1 and not b == 1", tags));
    EXPECT_TRUE(eval("a == 2 OR true", tags));
    EXPECT_FALSE(eval("false", tags));
    EXPECT_TRUE(eval("!!true", tags));
}

TEST(PredicateParserTest, WhitespaceIsInsignificant) {
    EXPECT_TRUE(eval("  (level>=WARN)&&(x<3)  ", tags_of({{"x", "2"}}), LogLevel::Error));
}

TEST(PredicateParserTest, ParsedPredicateSurvivesCopy) {
    auto parsed = parse_predicate("x > 1");
    ASSERT_TRUE(parsed.has_value());
    const Predicate copy = *parsed;
    parsed = parse_predicate("false");
    EXPECT_TRUE(copy.evaluate(tags_of({{"x", "2"}}), LogLevel::Info));
}

// ── Error paths ──────────────────────────────────────────────────────────────

TEST(PredicateParserTest, ErrorsCarryCodeAndPosition) {
    const PredicateParseError err = parse_error("level >= WARN && sensor_id == ");
    EXPECT_EQ(err.code, make_error_code(LumenError::InvalidPredicate));
    EXPECT_EQ(err.position, 30u);
    EXPECT_FALSE(err.message.empty());
}

TEST(PredicateParserTest, EmptyQueryIsAnError) {
    EXPECT_EQ(parse_error("").position, 0u);
    EXPECT_EQ(parse_error("   ").position, 3u);
}

TEST(PredicateParserTest, RejectsMalformedQueries) {
    parse_error("level >= LOUD");        // unknown level
    parse_error("level WARN");           // missing operator
    parse_error("x = 1");                // single '='
    parse_error("x == 1 & y == 2");      // single '&'
    parse_error("x == 1 | y == 2");      // single '|'
    parse_error("(x == 1");              // unclosed paren
    parse_error("x == 1)");              // stray paren
    parse_error("x == \"open");          // unterminated string
    parse_error("x < warmup");           // ordering needs a number
    parse_error("x == 1.2.3");           // not a number
    parse_error("sensor_id");            // bare key
    parse_error("exists(x");             // unclosed exists
    parse_error("exists()");             // missing key
    parse_error("x == 1 y == 2");        // missing operator between conditions
    parse_error("x == 1 &&");            // dangling operator
    parse_error("x == 1 # comment");     // unknown character
}

TEST(PredicateParserTest, BareKeyErrorSuggestsExists) {
    const PredicateParseError err = parse_error("sensor_id && level >= WARN");
    EXPECT_NE(err.message.find("exists(sensor_id)"), std::string::npos) << err.message;
}

TEST(PredicateParserTest, DeepNestingIsRejectedNotCrashing) {
    std::string deep(10000, '(');
    deep += "true";
    deep += std::string(10000, ')');
    const PredicateParseError err = parse_error(deep);
    EXPECT_NE(err.message.find("nested"), std::string::npos) << err.message;

    std::string nots(10000, '!');
    nots += "true";
    parse_error(nots);
}

TEST(PredicateParserTest, ModerateNestingIsAccepted) {
    std::string query(32, '(');
    query += "true";
    query += std::string(32, ')');
    EXPECT_TRUE(eval(query, {}));
}

// ── Independence from the C locale ───────────────────────────────────────────

// Switches the global C locale to the first of `names` installed here, and
// back on destruction. active() is false if none of them is installed; most
// machines have only "C" and "C.UTF-8", and these tests then skip.
class ScopedCLocale {
public:
    explicit ScopedCLocale(std::initializer_list<const char*> names)
        : previous_(std::setlocale(LC_ALL, nullptr)) {
        for (const char* name : names) {
            if (std::setlocale(LC_ALL, name) != nullptr) {
                active_ = true;
                return;
            }
        }
    }
    // Restoring the name setlocale itself returned cannot fail.
    ~ScopedCLocale() { std::setlocale(LC_ALL, previous_.c_str()); }
    ScopedCLocale(const ScopedCLocale&) = delete;
    ScopedCLocale(ScopedCLocale&&) = delete;
    ScopedCLocale& operator=(const ScopedCLocale&) = delete;
    ScopedCLocale& operator=(ScopedCLocale&&) = delete;

    [[nodiscard]] bool active() const { return active_; }

private:
    std::string previous_;
    bool active_ = false;
};

void expect_rejects_latin1_letter() {
    // 0xE9 is 'e' with an acute accent in ISO-8859-1.
    const PredicateParseError err = parse_error("caf\xe9 == 1");
    EXPECT_EQ(err.position, 3u);
    EXPECT_EQ(err.message.rfind("unexpected character", 0), 0u) << err.message;
}

TEST(PredicateParserTest, NonAsciiLetterInKeyIsRejected) {
    expect_rejects_latin1_letter();
}

TEST(PredicateParserTest, NonAsciiLetterInKeyIsRejectedUnderLatin1Locale) {
    const ScopedCLocale latin1({"de_DE.ISO-8859-1", "de_DE.iso88591", "en_US.ISO-8859-1",
                                "en_US.iso88591"});
    if (!latin1.active()) GTEST_SKIP() << "no ISO-8859-1 locale installed";
    // There, std::isalpha(0xE9) is true.
    expect_rejects_latin1_letter();
}

TEST(PredicateParserTest, KeywordsMatchAnyCaseUnderTurkishLocale) {
    const ScopedCLocale turkish({"tr_TR.ISO-8859-9", "tr_TR.iso88599"});
    if (!turkish.active()) GTEST_SKIP() << "no ISO-8859-9 Turkish locale installed";
    // There, std::tolower('I') is the dotless i (0xFD), not 'i'.
    EXPECT_TRUE(eval("EXISTS(k)", tags_of({{"k", "1"}})));
    EXPECT_TRUE(eval("level == info", {}, LogLevel::Info));
}

}  // namespace
}  // namespace lumen
