#include <gtest/gtest.h>

#include "lumen/predicate.hpp"

namespace lumen {
namespace {

TagSet<16> make_tags() {
    TagSet<16> tags;
    tags.add("env", "prod");
    tags.add("app", "myapp");
    return tags;
}

// ── Always / Never ────────────────────────────────────────────────────────────

TEST(PredicateTest, AlwaysTrue) {
    Predicate p = always();
    TagSet<16> tags;
    EXPECT_TRUE(p.evaluate(tags, LogLevel::Trace));
    EXPECT_TRUE(p.evaluate(tags, LogLevel::Fatal));
}

TEST(PredicateTest, NeverFalse) {
    Predicate p = never();
    TagSet<16> tags;
    EXPECT_FALSE(p.evaluate(tags, LogLevel::Trace));
    EXPECT_FALSE(p.evaluate(tags, LogLevel::Fatal));
}

// ── LevelAtLeast ──────────────────────────────────────────────────────────────

TEST(PredicateTest, LevelAtLeastMatches) {
    Predicate p = level_at_least(LogLevel::Info);
    TagSet<16> tags;

    EXPECT_FALSE(p.evaluate(tags, LogLevel::Trace));
    EXPECT_FALSE(p.evaluate(tags, LogLevel::Debug));
    EXPECT_TRUE(p.evaluate(tags, LogLevel::Info));
    EXPECT_TRUE(p.evaluate(tags, LogLevel::Warn));
    EXPECT_TRUE(p.evaluate(tags, LogLevel::Error));
    EXPECT_TRUE(p.evaluate(tags, LogLevel::Fatal));
}

TEST(PredicateTest, LevelAtLeastMisses) {
    Predicate p = level_at_least(LogLevel::Error);
    TagSet<16> tags;

    EXPECT_FALSE(p.evaluate(tags, LogLevel::Trace));
    EXPECT_FALSE(p.evaluate(tags, LogLevel::Debug));
    EXPECT_FALSE(p.evaluate(tags, LogLevel::Info));
    EXPECT_FALSE(p.evaluate(tags, LogLevel::Warn));
    EXPECT_TRUE(p.evaluate(tags, LogLevel::Error));
    EXPECT_TRUE(p.evaluate(tags, LogLevel::Fatal));
}

// ── LevelEquals ───────────────────────────────────────────────────────────────

TEST(PredicateTest, LevelEquals) {
    Predicate p = level_equals(LogLevel::Warn);

    EXPECT_FALSE(p.evaluate(TagSet<16>{}, LogLevel::Info));
    EXPECT_TRUE(p.evaluate(TagSet<16>{}, LogLevel::Warn));
    EXPECT_FALSE(p.evaluate(TagSet<16>{}, LogLevel::Error));
}

// ── TagEquals ─────────────────────────────────────────────────────────────────

TEST(PredicateTest, TagEqualsMatch) {
    Predicate p = tag_equals("env", "prod");
    auto tags = make_tags();

    EXPECT_TRUE(p.evaluate(tags, LogLevel::Info));
}

TEST(PredicateTest, TagEqualsMiss) {
    Predicate p = tag_equals("env", "staging");
    auto tags = make_tags();

    EXPECT_FALSE(p.evaluate(tags, LogLevel::Info));
}

TEST(PredicateTest, TagEqualsUnknownKey) {
    Predicate p = tag_equals("unknown", "value");
    auto tags = make_tags();

    EXPECT_FALSE(p.evaluate(tags, LogLevel::Info));
}

// ── TagExists ─────────────────────────────────────────────────────────────────

TEST(PredicateTest, TagExistsMatch) {
    Predicate p = tag_exists("env");
    auto tags = make_tags();

    EXPECT_TRUE(p.evaluate(tags, LogLevel::Info));
}

TEST(PredicateTest, TagExistsMiss) {
    Predicate p = tag_exists("missing");
    auto tags = make_tags();

    EXPECT_FALSE(p.evaluate(tags, LogLevel::Info));
}

// ── And ───────────────────────────────────────────────────────────────────────

TEST(PredicateTest, AndBothTrue) {
    Predicate p = tag_equals("env", "prod") && level_at_least(LogLevel::Info);
    auto tags = make_tags();

    EXPECT_TRUE(p.evaluate(tags, LogLevel::Warn));
}

TEST(PredicateTest, AndLeftFalseShortCircuits) {
    Predicate p = never() && level_at_least(LogLevel::Info);
    auto tags = make_tags();

    EXPECT_FALSE(p.evaluate(tags, LogLevel::Warn));
}

TEST(PredicateTest, AndRightFalse) {
    Predicate p = level_at_least(LogLevel::Info) && tag_equals("env", "missing");
    auto tags = make_tags();

    EXPECT_FALSE(p.evaluate(tags, LogLevel::Warn));
}

// ── Or ────────────────────────────────────────────────────────────────────────

TEST(PredicateTest, OrLeftTrueShortCircuits) {
    Predicate p = always() || never();
    auto tags = make_tags();

    EXPECT_TRUE(p.evaluate(tags, LogLevel::Trace));
}

TEST(PredicateTest, OrRightTrue) {
    Predicate p = never() || always();
    auto tags = make_tags();

    EXPECT_TRUE(p.evaluate(tags, LogLevel::Trace));
}

TEST(PredicateTest, OrBothFalse) {
    Predicate p = never() || tag_equals("env", "missing");
    auto tags = make_tags();

    EXPECT_FALSE(p.evaluate(tags, LogLevel::Trace));
}

// ── Not ───────────────────────────────────────────────────────────────────────

TEST(PredicateTest, NotInvertsAlways) {
    Predicate p = !always();
    TagSet<16> tags;

    EXPECT_FALSE(p.evaluate(tags, LogLevel::Info));
}

TEST(PredicateTest, NotInvertsNever) {
    Predicate p = !never();
    TagSet<16> tags;

    EXPECT_TRUE(p.evaluate(tags, LogLevel::Info));
}

TEST(PredicateTest, NotInvertsTagEquals) {
    Predicate p = !tag_equals("env", "prod");
    auto tags = make_tags();

    EXPECT_FALSE(p.evaluate(tags, LogLevel::Info));
}

// ── Composite ─────────────────────────────────────────────────────────────────

TEST(PredicateTest, CompositeEveryCombination) {
    auto tags = make_tags();

    // (env=prod || app=other) && level>=INFO && !tag_exists(missing)
    Predicate p =
        (tag_equals("env", "prod") || tag_equals("app", "other")) &&
        level_at_least(LogLevel::Info) &&
        !tag_exists("missing");

    EXPECT_TRUE(p.evaluate(tags, LogLevel::Info));
    EXPECT_FALSE(p.evaluate(tags, LogLevel::Debug));
    EXPECT_TRUE(p.evaluate(tags, LogLevel::Error));

    // Missing tag condition fails
    Predicate p2 =
        (tag_equals("env", "prod") || tag_equals("app", "other")) &&
        tag_exists("missing");
    EXPECT_FALSE(p2.evaluate(tags, LogLevel::Info));
}

TEST(PredicateTest, DeepNesting) {
    auto tags = make_tags();

    // !((never() && always()) || (level_at_least(INFO) && !tag_equals("env", "prod")))
    Predicate p = !(
        (never() && always()) ||
        (level_at_least(LogLevel::Info) && !tag_equals("env", "prod"))
    );

    // Inner: never()&&always()=false, so || depends on right side
    // Right: level>=INFO(true for INFO) && !env=prod(false for "prod") = false
    // Or: false || false = false
    // Not: true
    EXPECT_TRUE(p.evaluate(tags, LogLevel::Info));
}

// ── Copy semantics ────────────────────────────────────────────────────────────

TEST(PredicateTest, CopyPreservesBehavior) {
    Predicate p1 = tag_equals("env", "prod") && level_at_least(LogLevel::Info);
    Predicate p2(p1);
    Predicate p3 = p1;

    auto tags = make_tags();
    EXPECT_TRUE(p1.evaluate(tags, LogLevel::Warn));
    EXPECT_TRUE(p2.evaluate(tags, LogLevel::Warn));
    EXPECT_TRUE(p3.evaluate(tags, LogLevel::Warn));

    EXPECT_FALSE(p1.evaluate(tags, LogLevel::Debug));
    EXPECT_FALSE(p2.evaluate(tags, LogLevel::Debug));
    EXPECT_FALSE(p3.evaluate(tags, LogLevel::Debug));
}

TEST(PredicateTest, MovePreservesBehavior) {
    Predicate p1 = tag_equals("env", "prod");
    Predicate p2(std::move(p1));
    Predicate p3 = std::move(p2);

    auto tags = make_tags();
    EXPECT_TRUE(p3.evaluate(tags, LogLevel::Info));
}

TEST(PredicateTest, AssignmentPreservesBehavior) {
    Predicate p1 = tag_equals("env", "prod");
    Predicate p2 = never();
    p2 = p1;

    auto tags = make_tags();
    EXPECT_TRUE(p2.evaluate(tags, LogLevel::Info));

    TagSet<16> empty;
    EXPECT_FALSE(p2.evaluate(empty, LogLevel::Info));
}

// ── Error-path tests ──────────────────────────────────────────────────────────

TEST(PredicateTest, DefaultPredicateEvaluatesTrue) {
    Predicate p;
    TagSet<16> tags;
    EXPECT_TRUE(p.evaluate(tags, LogLevel::Trace));
}

TEST(PredicateTest, EmptyTagSetEvaluate) {
    Predicate p = tag_equals("env", "prod") && level_at_least(LogLevel::Info);
    TagSet<16> empty;

    EXPECT_FALSE(p.evaluate(empty, LogLevel::Info));
    EXPECT_FALSE(p.evaluate(empty, LogLevel::Warn));
}

TEST(PredicateTest, MaximumLevelEvaluate) {
    Predicate p = level_at_least(LogLevel::Fatal);
    TagSet<16> tags;

    EXPECT_FALSE(p.evaluate(tags, LogLevel::Trace));
    EXPECT_FALSE(p.evaluate(tags, LogLevel::Info));
    EXPECT_TRUE(p.evaluate(tags, LogLevel::Fatal));
}

TEST(PredicateTest, NeverCombinedWithOr) {
    Predicate p = never() || tag_equals("env", "prod");
    auto tags = make_tags();
    EXPECT_TRUE(p.evaluate(tags, LogLevel::Trace));

    Predicate p2 = never() || never();
    EXPECT_FALSE(p2.evaluate(tags, LogLevel::Trace));
}

TEST(PredicateTest, AlwaysCombinedWithAnd) {
    Predicate p = always() && tag_equals("env", "prod");
    auto tags = make_tags();
    EXPECT_TRUE(p.evaluate(tags, LogLevel::Trace));

    Predicate p2 = always() && always();
    EXPECT_TRUE(p2.evaluate(tags, LogLevel::Trace));
}

// ── TagExists with empty value ───────────────────────────────────────────────

TEST(PredicateTest, TagExistsMatchesEmptyValue) {
    TagSet<16> tags;
    tags.add("flag", "");
    EXPECT_TRUE(tag_exists("flag").evaluate(tags, LogLevel::Info));
}

// ── Numeric tag comparisons ──────────────────────────────────────────────────

TEST(PredicateTest, TagLessAndGreater) {
    TagSet<16> tags;
    tags.add("altitude", "69999.97");

    EXPECT_TRUE(tag_less("altitude", 70000).evaluate(tags, LogLevel::Info));
    EXPECT_FALSE(tag_greater("altitude", 70000).evaluate(tags, LogLevel::Info));
    EXPECT_TRUE(tag_less_equal("altitude", 69999.97).evaluate(tags, LogLevel::Info));
    EXPECT_TRUE(tag_greater_equal("altitude", 69999.97).evaluate(tags, LogLevel::Info));
    EXPECT_FALSE(tag_less("altitude", 69999.97).evaluate(tags, LogLevel::Info));
}

TEST(PredicateTest, TagCompareParsesIntegersNegativesAndExponents) {
    TagSet<16> tags;
    tags.add("count", "42");
    tags.add("temp", "-12.5");
    tags.add("eps", "1e-06");

    EXPECT_TRUE(tag_compare("count", CompareOp::Equal, 42.0).evaluate(tags, LogLevel::Info));
    EXPECT_TRUE(tag_compare("count", CompareOp::NotEqual, 41.0).evaluate(tags, LogLevel::Info));
    EXPECT_TRUE(tag_less("temp", -10).evaluate(tags, LogLevel::Info));
    EXPECT_TRUE(tag_less("eps", 1e-5).evaluate(tags, LogLevel::Info));
}

TEST(PredicateTest, TagCompareMissingOrNonNumericIsFalse) {
    TagSet<16> tags;
    tags.add("name", "rocket");
    tags.add("partial", "42abc");
    tags.add("empty", "");

    for (const char* key : {"name", "partial", "empty", "missing"}) {
        for (CompareOp op : {CompareOp::Equal, CompareOp::NotEqual, CompareOp::Less, CompareOp::LessEqual,
                             CompareOp::Greater, CompareOp::GreaterEqual}) {
            EXPECT_FALSE(tag_compare(key, op, 0.0).evaluate(tags, LogLevel::Info))
                << "key=" << key << " op=" << static_cast<int>(op);
        }
    }
}

TEST(PredicateTest, TagCompareComposesWithLevel) {
    TagSet<16> tags;
    tags.add("altitude", "500");
    const Predicate p = level_at_least(LogLevel::Warn) && tag_less("altitude", 1000);

    EXPECT_TRUE(p.evaluate(tags, LogLevel::Error));
    EXPECT_FALSE(p.evaluate(tags, LogLevel::Info));
}

TEST(PredicateTest, LevelCompareAllOperators) {
    const TagSet<16> tags;
    EXPECT_TRUE(level_compare(CompareOp::Less, LogLevel::Warn).evaluate(tags, LogLevel::Info));
    EXPECT_FALSE(level_compare(CompareOp::Less, LogLevel::Warn).evaluate(tags, LogLevel::Warn));
    EXPECT_TRUE(level_compare(CompareOp::LessEqual, LogLevel::Warn).evaluate(tags, LogLevel::Warn));
    EXPECT_TRUE(level_compare(CompareOp::Greater, LogLevel::Warn).evaluate(tags, LogLevel::Error));
    EXPECT_TRUE(level_compare(CompareOp::GreaterEqual, LogLevel::Warn).evaluate(tags, LogLevel::Warn));
    EXPECT_TRUE(level_compare(CompareOp::Equal, LogLevel::Warn).evaluate(tags, LogLevel::Warn));
    EXPECT_TRUE(level_compare(CompareOp::NotEqual, LogLevel::Warn).evaluate(tags, LogLevel::Info));
}

// ── Records without a level (metrics, progress) ──────────────────────────────

TEST(PredicateTest, LevelOnlyPredicatesPassRecordsWithoutLevel) {
    const TagSet<16> tags;
    EXPECT_TRUE(level_at_least(LogLevel::Fatal).evaluate(tags));
    EXPECT_TRUE(level_equals(LogLevel::Error).evaluate(tags));
    EXPECT_TRUE(level_compare(CompareOp::Less, LogLevel::Trace).evaluate(tags));
    EXPECT_TRUE((!level_at_least(LogLevel::Warn)).evaluate(tags));
    EXPECT_TRUE((level_at_least(LogLevel::Warn) && level_equals(LogLevel::Info)).evaluate(tags));
}

TEST(PredicateTest, LevelConditionIsIgnoredInsideAnd) {
    TagSet<16> match;
    match.add("vessel_id", "42");
    TagSet<16> other;
    other.add("vessel_id", "7");
    const Predicate p = level_at_least(LogLevel::Warn) && tag_compare("vessel_id", CompareOp::Equal, 42);

    EXPECT_TRUE(p.evaluate(match));
    EXPECT_FALSE(p.evaluate(other));
}

TEST(PredicateTest, LevelConditionIsIgnoredInsideOr) {
    // Treating the level part as "true" would pass every metric here.
    TagSet<16> other;
    other.add("vessel_id", "7");
    const Predicate p = level_at_least(LogLevel::Warn) || tag_compare("vessel_id", CompareOp::Equal, 42);

    EXPECT_FALSE(p.evaluate(other));
}

TEST(PredicateTest, NegatedLevelConditionIsIgnored) {
    TagSet<16> tags;
    tags.add("suppress", "1");
    EXPECT_FALSE((!level_at_least(LogLevel::Warn) && !tag_exists("suppress")).evaluate(tags));
    EXPECT_TRUE((!level_at_least(LogLevel::Warn) || tag_exists("suppress")).evaluate(tags));
}

TEST(PredicateTest, TagOnlyPredicatesBehaveTheSameWithOrWithoutLevel) {
    TagSet<16> tags;
    tags.add("env", "prod");
    const Predicate p = tag_equals("env", "prod") && !tag_exists("suppress");
    EXPECT_EQ(p.evaluate(tags), p.evaluate(tags, LogLevel::Trace));
    EXPECT_FALSE(never().evaluate(tags));
}

TEST(PredicateTest, LevelStillFiltersLogRecords) {
    const TagSet<16> tags;
    EXPECT_FALSE(level_at_least(LogLevel::Warn).evaluate(tags, LogLevel::Info));
    EXPECT_FALSE((!level_at_least(LogLevel::Info)).evaluate(tags, LogLevel::Warn));
}

}  // namespace
}  // namespace lumen
