#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "lumen/detail/error.h"
#include "lumen/predicate.h"

namespace lumen {
namespace {

TagSet<16> tags_of(std::initializer_list<std::pair<std::string_view, std::string_view>> kv) {
    TagSet<16> tags;
    for (const auto& [k, v] : kv) tags.add(k, v);
    return tags;
}

// Parses `query` (failing the test on a parse error) and evaluates it.
bool eval(std::string_view query, const TagSet<16>& tags, LogLevel level = INFO) {
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

// ── The Helios example ───────────────────────────────────────────────────────

TEST(PredicateParserTest, HeliosExampleQuery) {
    constexpr std::string_view query = "level >= WARN && vessel_id == 42 && !exists(suppress)";
    const auto vessel = tags_of({{"vessel_id", "42"}});
    const auto suppressed = tags_of({{"vessel_id", "42"}, {"suppress", "1"}});
    const auto other = tags_of({{"vessel_id", "7"}});

    EXPECT_TRUE(eval(query, vessel, WARN));
    EXPECT_TRUE(eval(query, vessel, FATAL));
    EXPECT_FALSE(eval(query, vessel, INFO));
    EXPECT_FALSE(eval(query, suppressed, ERROR));
    EXPECT_FALSE(eval(query, other, ERROR));
}

// ── Level comparisons ────────────────────────────────────────────────────────

TEST(PredicateParserTest, LevelOperators) {
    const TagSet<16> none;
    EXPECT_TRUE(eval("level == INFO", none, INFO));
    EXPECT_FALSE(eval("level != INFO", none, INFO));
    EXPECT_TRUE(eval("level < WARN", none, INFO));
    EXPECT_FALSE(eval("level <= DEBUG", none, INFO));
    EXPECT_TRUE(eval("level > DEBUG", none, INFO));
    EXPECT_TRUE(eval("level >= INFO", none, INFO));
}

TEST(PredicateParserTest, LevelNamesAreCaseInsensitiveAndNumericLevelsWork) {
    const TagSet<16> none;
    EXPECT_TRUE(eval("level >= warn", none, ERROR));
    EXPECT_TRUE(eval("LEVEL == Error", none, ERROR));
    EXPECT_TRUE(eval("level == 4", none, ERROR));
}

// ── Tag comparisons ──────────────────────────────────────────────────────────

TEST(PredicateParserTest, NumericEqualityIgnoresSpelling) {
    EXPECT_TRUE(eval("vessel_id == 42", tags_of({{"vessel_id", "42.0"}})));
    EXPECT_TRUE(eval("gain == 1e3", tags_of({{"gain", "1000"}})));
    EXPECT_FALSE(eval("vessel_id == 42", tags_of({{"vessel_id", "42abc"}})));
}

TEST(PredicateParserTest, NumericOrdering) {
    const auto tags = tags_of({{"altitude", "69999.97"}, {"temp", "-12.5"}});
    EXPECT_TRUE(eval("altitude < 70000", tags));
    EXPECT_FALSE(eval("altitude >= 70000", tags));
    EXPECT_TRUE(eval("temp < -10", tags));
    EXPECT_TRUE(eval("temp > -1e2", tags));
    EXPECT_TRUE(eval("altitude <= +69999.97", tags));
}

TEST(PredicateParserTest, StringComparisons) {
    const auto tags = tags_of({{"phase", "ascent"}, {"name", "Kerbal X"}});
    EXPECT_TRUE(eval("phase == ascent", tags));
    EXPECT_TRUE(eval("phase == \"ascent\"", tags));
    EXPECT_TRUE(eval("name == 'Kerbal X'", tags));
    EXPECT_TRUE(eval("phase != orbit", tags));
    EXPECT_FALSE(eval("phase != ascent", tags));
}

TEST(PredicateParserTest, EscapedQuotesInStrings) {
    EXPECT_TRUE(eval(R"(msg == "say \"hi\"")", tags_of({{"msg", "say \"hi\""}})));
}

TEST(PredicateParserTest, ComparisonsAgainstMissingTagAreFalse) {
    const TagSet<16> none;
    EXPECT_FALSE(eval("altitude < 70000", none));
    EXPECT_FALSE(eval("altitude != 5", none));
    EXPECT_FALSE(eval("phase != orbit", none));
    EXPECT_TRUE(eval("!exists(phase) || phase != orbit", none));
}

TEST(PredicateParserTest, ExistsMatchesTagWithEmptyValue) {
    EXPECT_TRUE(eval("exists(flag)", tags_of({{"flag", ""}})));
    EXPECT_FALSE(eval("exists(flag)", tags_of({{"other", "x"}})));
}

TEST(PredicateParserTest, QuotedKeyReachesTagNamedLikeKeyword) {
    EXPECT_TRUE(eval("\"level\" == 3", tags_of({{"level", "3"}}), TRACE));
    EXPECT_TRUE(eval("exists(\"and\")", tags_of({{"and", "x"}})));
}

TEST(PredicateParserTest, DottedKeys) {
    EXPECT_TRUE(eval("vessel.id == 42", tags_of({{"vessel.id", "42"}})));
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
    EXPECT_TRUE(eval("  (level>=WARN)&&(x<3)  ", tags_of({{"x", "2"}}), ERROR));
}

TEST(PredicateParserTest, ParsedPredicateSurvivesCopy) {
    auto parsed = parse_predicate("x > 1");
    ASSERT_TRUE(parsed.has_value());
    const Predicate copy = *parsed;
    parsed = parse_predicate("false");
    EXPECT_TRUE(copy.evaluate(tags_of({{"x", "2"}}), INFO));
}

// ── Error paths ──────────────────────────────────────────────────────────────

TEST(PredicateParserTest, ErrorsCarryCodeAndPosition) {
    const PredicateParseError err = parse_error("level >= WARN && vessel_id == ");
    EXPECT_EQ(err.code, make_error_code(LumenError::invalid_predicate));
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
    parse_error("x < ascent");           // ordering needs a number
    parse_error("x == 1.2.3");           // not a number
    parse_error("vessel_id");            // bare key
    parse_error("exists(x");             // unclosed exists
    parse_error("exists()");             // missing key
    parse_error("x == 1 y == 2");        // missing operator between conditions
    parse_error("x == 1 &&");            // dangling operator
    parse_error("x == 1 # comment");     // unknown character
}

TEST(PredicateParserTest, BareKeyErrorSuggestsExists) {
    const PredicateParseError err = parse_error("vessel_id && level >= WARN");
    EXPECT_NE(err.message.find("exists(vessel_id)"), std::string::npos) << err.message;
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

}  // namespace
}  // namespace lumen
