#include <gtest/gtest.h>

#include "lumen/predicate.h"

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
    EXPECT_TRUE(p.evaluate(tags, LogLevel::TRACE));
    EXPECT_TRUE(p.evaluate(tags, LogLevel::FATAL));
}

TEST(PredicateTest, NeverFalse) {
    Predicate p = never();
    TagSet<16> tags;
    EXPECT_FALSE(p.evaluate(tags, LogLevel::TRACE));
    EXPECT_FALSE(p.evaluate(tags, LogLevel::FATAL));
}

// ── LevelAtLeast ──────────────────────────────────────────────────────────────

TEST(PredicateTest, LevelAtLeastMatches) {
    Predicate p = level_at_least(LogLevel::INFO);
    TagSet<16> tags;

    EXPECT_FALSE(p.evaluate(tags, LogLevel::TRACE));
    EXPECT_FALSE(p.evaluate(tags, LogLevel::DEBUG));
    EXPECT_TRUE(p.evaluate(tags, LogLevel::INFO));
    EXPECT_TRUE(p.evaluate(tags, LogLevel::WARN));
    EXPECT_TRUE(p.evaluate(tags, LogLevel::ERROR));
    EXPECT_TRUE(p.evaluate(tags, LogLevel::FATAL));
}

TEST(PredicateTest, LevelAtLeastMisses) {
    Predicate p = level_at_least(LogLevel::ERROR);
    TagSet<16> tags;

    EXPECT_FALSE(p.evaluate(tags, LogLevel::TRACE));
    EXPECT_FALSE(p.evaluate(tags, LogLevel::DEBUG));
    EXPECT_FALSE(p.evaluate(tags, LogLevel::INFO));
    EXPECT_FALSE(p.evaluate(tags, LogLevel::WARN));
    EXPECT_TRUE(p.evaluate(tags, LogLevel::ERROR));
    EXPECT_TRUE(p.evaluate(tags, LogLevel::FATAL));
}

// ── LevelEquals ───────────────────────────────────────────────────────────────

TEST(PredicateTest, LevelEquals) {
    Predicate p = level_equals(LogLevel::WARN);

    EXPECT_FALSE(p.evaluate(TagSet<16>{}, LogLevel::INFO));
    EXPECT_TRUE(p.evaluate(TagSet<16>{}, LogLevel::WARN));
    EXPECT_FALSE(p.evaluate(TagSet<16>{}, LogLevel::ERROR));
}

// ── TagEquals ─────────────────────────────────────────────────────────────────

TEST(PredicateTest, TagEqualsMatch) {
    Predicate p = tag_equals("env", "prod");
    auto tags = make_tags();

    EXPECT_TRUE(p.evaluate(tags, LogLevel::INFO));
}

TEST(PredicateTest, TagEqualsMiss) {
    Predicate p = tag_equals("env", "staging");
    auto tags = make_tags();

    EXPECT_FALSE(p.evaluate(tags, LogLevel::INFO));
}

TEST(PredicateTest, TagEqualsUnknownKey) {
    Predicate p = tag_equals("unknown", "value");
    auto tags = make_tags();

    EXPECT_FALSE(p.evaluate(tags, LogLevel::INFO));
}

// ── TagExists ─────────────────────────────────────────────────────────────────

TEST(PredicateTest, TagExistsMatch) {
    Predicate p = tag_exists("env");
    auto tags = make_tags();

    EXPECT_TRUE(p.evaluate(tags, LogLevel::INFO));
}

TEST(PredicateTest, TagExistsMiss) {
    Predicate p = tag_exists("missing");
    auto tags = make_tags();

    EXPECT_FALSE(p.evaluate(tags, LogLevel::INFO));
}

// ── And ───────────────────────────────────────────────────────────────────────

TEST(PredicateTest, AndBothTrue) {
    Predicate p = tag_equals("env", "prod") && level_at_least(LogLevel::INFO);
    auto tags = make_tags();

    EXPECT_TRUE(p.evaluate(tags, LogLevel::WARN));
}

TEST(PredicateTest, AndLeftFalseShortCircuits) {
    Predicate p = never() && level_at_least(LogLevel::INFO);
    auto tags = make_tags();

    EXPECT_FALSE(p.evaluate(tags, LogLevel::WARN));
}

TEST(PredicateTest, AndRightFalse) {
    Predicate p = level_at_least(LogLevel::INFO) && tag_equals("env", "missing");
    auto tags = make_tags();

    EXPECT_FALSE(p.evaluate(tags, LogLevel::WARN));
}

// ── Or ────────────────────────────────────────────────────────────────────────

TEST(PredicateTest, OrLeftTrueShortCircuits) {
    Predicate p = always() || never();
    auto tags = make_tags();

    EXPECT_TRUE(p.evaluate(tags, LogLevel::TRACE));
}

TEST(PredicateTest, OrRightTrue) {
    Predicate p = never() || always();
    auto tags = make_tags();

    EXPECT_TRUE(p.evaluate(tags, LogLevel::TRACE));
}

TEST(PredicateTest, OrBothFalse) {
    Predicate p = never() || tag_equals("env", "missing");
    auto tags = make_tags();

    EXPECT_FALSE(p.evaluate(tags, LogLevel::TRACE));
}

// ── Not ───────────────────────────────────────────────────────────────────────

TEST(PredicateTest, NotInvertsAlways) {
    Predicate p = !always();
    TagSet<16> tags;

    EXPECT_FALSE(p.evaluate(tags, LogLevel::INFO));
}

TEST(PredicateTest, NotInvertsNever) {
    Predicate p = !never();
    TagSet<16> tags;

    EXPECT_TRUE(p.evaluate(tags, LogLevel::INFO));
}

TEST(PredicateTest, NotInvertsTagEquals) {
    Predicate p = !tag_equals("env", "prod");
    auto tags = make_tags();

    EXPECT_FALSE(p.evaluate(tags, LogLevel::INFO));
}

// ── Composite ─────────────────────────────────────────────────────────────────

TEST(PredicateTest, CompositeEveryCombination) {
    auto tags = make_tags();

    // (env=prod || app=other) && level>=INFO && !tag_exists(missing)
    Predicate p =
        (tag_equals("env", "prod") || tag_equals("app", "other")) &&
        level_at_least(LogLevel::INFO) &&
        !tag_exists("missing");

    EXPECT_TRUE(p.evaluate(tags, LogLevel::INFO));
    EXPECT_FALSE(p.evaluate(tags, LogLevel::DEBUG));
    EXPECT_TRUE(p.evaluate(tags, LogLevel::ERROR));

    // Missing tag condition fails
    Predicate p2 =
        (tag_equals("env", "prod") || tag_equals("app", "other")) &&
        tag_exists("missing");
    EXPECT_FALSE(p2.evaluate(tags, LogLevel::INFO));
}

TEST(PredicateTest, DeepNesting) {
    auto tags = make_tags();

    // !((never() && always()) || (level_at_least(INFO) && !tag_equals("env", "prod")))
    Predicate p = !(
        (never() && always()) ||
        (level_at_least(LogLevel::INFO) && !tag_equals("env", "prod"))
    );

    // Inner: never()&&always()=false, so || depends on right side
    // Right: level>=INFO(true for INFO) && !env=prod(false for "prod") = false
    // Or: false || false = false
    // Not: true
    EXPECT_TRUE(p.evaluate(tags, LogLevel::INFO));
}

// ── Copy semantics ────────────────────────────────────────────────────────────

TEST(PredicateTest, CopyPreservesBehavior) {
    Predicate p1 = tag_equals("env", "prod") && level_at_least(LogLevel::INFO);
    Predicate p2(p1);
    Predicate p3 = p1;

    auto tags = make_tags();
    EXPECT_TRUE(p1.evaluate(tags, LogLevel::WARN));
    EXPECT_TRUE(p2.evaluate(tags, LogLevel::WARN));
    EXPECT_TRUE(p3.evaluate(tags, LogLevel::WARN));

    EXPECT_FALSE(p1.evaluate(tags, LogLevel::DEBUG));
    EXPECT_FALSE(p2.evaluate(tags, LogLevel::DEBUG));
    EXPECT_FALSE(p3.evaluate(tags, LogLevel::DEBUG));
}

TEST(PredicateTest, MovePreservesBehavior) {
    Predicate p1 = tag_equals("env", "prod");
    Predicate p2(std::move(p1));
    Predicate p3 = std::move(p2);

    auto tags = make_tags();
    EXPECT_TRUE(p3.evaluate(tags, LogLevel::INFO));
}

TEST(PredicateTest, AssignmentPreservesBehavior) {
    Predicate p1 = tag_equals("env", "prod");
    Predicate p2 = never();
    p2 = p1;

    auto tags = make_tags();
    EXPECT_TRUE(p2.evaluate(tags, LogLevel::INFO));

    TagSet<16> empty;
    EXPECT_FALSE(p2.evaluate(empty, LogLevel::INFO));
}

// ── Error-path tests ──────────────────────────────────────────────────────────

TEST(PredicateTest, DefaultPredicateEvaluatesTrue) {
    Predicate p;
    TagSet<16> tags;
    EXPECT_TRUE(p.evaluate(tags, LogLevel::TRACE));
}

TEST(PredicateTest, EmptyTagSetEvaluate) {
    Predicate p = tag_equals("env", "prod") && level_at_least(LogLevel::INFO);
    TagSet<16> empty;

    EXPECT_FALSE(p.evaluate(empty, LogLevel::INFO));
    EXPECT_FALSE(p.evaluate(empty, LogLevel::WARN));
}

TEST(PredicateTest, MaximumLevelEvaluate) {
    Predicate p = level_at_least(LogLevel::FATAL);
    TagSet<16> tags;

    EXPECT_FALSE(p.evaluate(tags, LogLevel::TRACE));
    EXPECT_FALSE(p.evaluate(tags, LogLevel::INFO));
    EXPECT_TRUE(p.evaluate(tags, LogLevel::FATAL));
}

TEST(PredicateTest, NeverCombinedWithOr) {
    Predicate p = never() || tag_equals("env", "prod");
    auto tags = make_tags();
    EXPECT_TRUE(p.evaluate(tags, LogLevel::TRACE));

    Predicate p2 = never() || never();
    EXPECT_FALSE(p2.evaluate(tags, LogLevel::TRACE));
}

TEST(PredicateTest, AlwaysCombinedWithAnd) {
    Predicate p = always() && tag_equals("env", "prod");
    auto tags = make_tags();
    EXPECT_TRUE(p.evaluate(tags, LogLevel::TRACE));

    Predicate p2 = always() && always();
    EXPECT_TRUE(p2.evaluate(tags, LogLevel::TRACE));
}

}  // namespace
}  // namespace lumen
