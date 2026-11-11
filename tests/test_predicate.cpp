#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <thread>
#include <utility>

#include <gtest/gtest.h>

#include "lumen/predicate.hpp"

namespace lumen {
namespace {

TagSet<16> make_tags() {
    TagSet<16> tags;
    EXPECT_TRUE(tags.add("env", "prod"));
    EXPECT_TRUE(tags.add("app", "myapp"));
    return tags;
}

inline constexpr std::array<CompareOp, 6> all_ops = {CompareOp::Equal,   CompareOp::NotEqual,
                                                     CompareOp::Less,    CompareOp::LessEqual,
                                                     CompareOp::Greater, CompareOp::GreaterEqual};

// Always / Never.

TEST(PredicateTest, AlwaysTrue) {
    const Predicate p = always();
    const TagSet<16> tags;
    EXPECT_TRUE(p.evaluate(tags, LogLevel::Trace));
    EXPECT_TRUE(p.evaluate(tags, LogLevel::Fatal));
}

TEST(PredicateTest, NeverFalse) {
    const Predicate p = never();
    const TagSet<16> tags;
    EXPECT_FALSE(p.evaluate(tags, LogLevel::Trace));
    EXPECT_FALSE(p.evaluate(tags, LogLevel::Fatal));
}

// LevelAtLeast.

TEST(PredicateTest, LevelAtLeastMatches) {
    const Predicate p = level_at_least(LogLevel::Info);
    const TagSet<16> tags;

    EXPECT_FALSE(p.evaluate(tags, LogLevel::Trace));
    EXPECT_FALSE(p.evaluate(tags, LogLevel::Debug));
    EXPECT_TRUE(p.evaluate(tags, LogLevel::Info));
    EXPECT_TRUE(p.evaluate(tags, LogLevel::Warn));
    EXPECT_TRUE(p.evaluate(tags, LogLevel::Error));
    EXPECT_TRUE(p.evaluate(tags, LogLevel::Fatal));
}

TEST(PredicateTest, LevelAtLeastMisses) {
    const Predicate p = level_at_least(LogLevel::Error);
    const TagSet<16> tags;

    EXPECT_FALSE(p.evaluate(tags, LogLevel::Trace));
    EXPECT_FALSE(p.evaluate(tags, LogLevel::Debug));
    EXPECT_FALSE(p.evaluate(tags, LogLevel::Info));
    EXPECT_FALSE(p.evaluate(tags, LogLevel::Warn));
    EXPECT_TRUE(p.evaluate(tags, LogLevel::Error));
    EXPECT_TRUE(p.evaluate(tags, LogLevel::Fatal));
}

TEST(PredicateTest, LevelAtLeastFatalPassesOnlyFatal) {
    const Predicate p = level_at_least(LogLevel::Fatal);
    const TagSet<16> tags;

    EXPECT_FALSE(p.evaluate(tags, LogLevel::Trace));
    EXPECT_FALSE(p.evaluate(tags, LogLevel::Info));
    EXPECT_FALSE(p.evaluate(tags, LogLevel::Error));
    EXPECT_TRUE(p.evaluate(tags, LogLevel::Fatal));
}

// LevelEquals.

TEST(PredicateTest, LevelEquals) {
    const Predicate p = level_equals(LogLevel::Warn);

    EXPECT_FALSE(p.evaluate(TagSet<16>{}, LogLevel::Info));
    EXPECT_TRUE(p.evaluate(TagSet<16>{}, LogLevel::Warn));
    EXPECT_FALSE(p.evaluate(TagSet<16>{}, LogLevel::Error));
}

TEST(PredicateTest, LevelCompareAllOperators) {
    const TagSet<16> tags;
    EXPECT_TRUE(level_compare(CompareOp::Less, LogLevel::Warn).evaluate(tags, LogLevel::Info));
    EXPECT_FALSE(level_compare(CompareOp::Less, LogLevel::Warn).evaluate(tags, LogLevel::Warn));
    EXPECT_TRUE(level_compare(CompareOp::LessEqual, LogLevel::Warn).evaluate(tags, LogLevel::Warn));
    EXPECT_FALSE(
        level_compare(CompareOp::LessEqual, LogLevel::Warn).evaluate(tags, LogLevel::Error));
    EXPECT_TRUE(level_compare(CompareOp::Greater, LogLevel::Warn).evaluate(tags, LogLevel::Error));
    EXPECT_FALSE(level_compare(CompareOp::Greater, LogLevel::Warn).evaluate(tags, LogLevel::Warn));
    EXPECT_TRUE(
        level_compare(CompareOp::GreaterEqual, LogLevel::Warn).evaluate(tags, LogLevel::Warn));
    EXPECT_FALSE(
        level_compare(CompareOp::GreaterEqual, LogLevel::Warn).evaluate(tags, LogLevel::Info));
    EXPECT_TRUE(level_compare(CompareOp::Equal, LogLevel::Warn).evaluate(tags, LogLevel::Warn));
    EXPECT_FALSE(level_compare(CompareOp::Equal, LogLevel::Warn).evaluate(tags, LogLevel::Info));
    EXPECT_TRUE(level_compare(CompareOp::NotEqual, LogLevel::Warn).evaluate(tags, LogLevel::Info));
    EXPECT_FALSE(level_compare(CompareOp::NotEqual, LogLevel::Warn).evaluate(tags, LogLevel::Warn));
}

// TagEquals / TagNotEquals.

TEST(PredicateTest, TagEqualsMatch) {
    const Predicate p = tag_equals("env", "prod");
    auto tags = make_tags();

    EXPECT_TRUE(p.evaluate(tags, LogLevel::Info));
}

TEST(PredicateTest, TagEqualsMiss) {
    const Predicate p = tag_equals("env", "staging");
    auto tags = make_tags();

    EXPECT_FALSE(p.evaluate(tags, LogLevel::Info));
}

TEST(PredicateTest, TagEqualsUnknownKey) {
    const Predicate p = tag_equals("unknown", "value");
    auto tags = make_tags();

    EXPECT_FALSE(p.evaluate(tags, LogLevel::Info));
}

TEST(PredicateTest, TagNotEqualsComparesPresentValue) {
    const auto tags = make_tags();
    EXPECT_TRUE(tag_not_equals("env", "staging").evaluate(tags, LogLevel::Info));
    EXPECT_FALSE(tag_not_equals("env", "prod").evaluate(tags, LogLevel::Info));
}

TEST(PredicateTest, TagEqualsEmptyStringMatchesPresentEmptyValue) {
    TagSet<16> tags;
    EXPECT_TRUE(tags.add("k", ""));
    EXPECT_TRUE(tag_equals("k", "").evaluate(tags, LogLevel::Info));
    EXPECT_FALSE(tag_not_equals("k", "").evaluate(tags, LogLevel::Info));
    EXPECT_TRUE(tag_not_equals("k", "x").evaluate(tags, LogLevel::Info));
}

// TagExists.

TEST(PredicateTest, TagExistsMatch) {
    const Predicate p = tag_exists("env");
    auto tags = make_tags();

    EXPECT_TRUE(p.evaluate(tags, LogLevel::Info));
}

TEST(PredicateTest, TagExistsMiss) {
    const Predicate p = tag_exists("missing");
    auto tags = make_tags();

    EXPECT_FALSE(p.evaluate(tags, LogLevel::Info));
}

TEST(PredicateTest, TagExistsMatchesEmptyValue) {
    TagSet<16> tags;
    EXPECT_TRUE(tags.add("flag", ""));
    EXPECT_TRUE(tag_exists("flag").evaluate(tags, LogLevel::Info));
}

// Numeric tag comparisons.

TEST(PredicateTest, TagLessAndGreater) {
    TagSet<16> tags;
    EXPECT_TRUE(tags.add("pressure", "69999.97"));

    EXPECT_TRUE(tag_less("pressure", 70000).evaluate(tags, LogLevel::Info));
    EXPECT_FALSE(tag_greater("pressure", 70000).evaluate(tags, LogLevel::Info));
    EXPECT_TRUE(tag_less_equal("pressure", 69999.97).evaluate(tags, LogLevel::Info));
    EXPECT_TRUE(tag_greater_equal("pressure", 69999.97).evaluate(tags, LogLevel::Info));
    EXPECT_FALSE(tag_less("pressure", 69999.97).evaluate(tags, LogLevel::Info));
}

TEST(PredicateTest, TagCompareParsesIntegersNegativesAndExponents) {
    TagSet<16> tags;
    EXPECT_TRUE(tags.add("count", "42"));
    EXPECT_TRUE(tags.add("temp", "-12.5"));
    EXPECT_TRUE(tags.add("eps", "1e-06"));

    EXPECT_TRUE(tag_compare("count", CompareOp::Equal, 42.0).evaluate(tags, LogLevel::Info));
    EXPECT_TRUE(tag_compare("count", CompareOp::NotEqual, 41.0).evaluate(tags, LogLevel::Info));
    EXPECT_TRUE(tag_less("temp", -10).evaluate(tags, LogLevel::Info));
    EXPECT_TRUE(tag_less("eps", 1e-5).evaluate(tags, LogLevel::Info));
}

TEST(PredicateTest, TagCompareComposesWithLevel) {
    TagSet<16> tags;
    EXPECT_TRUE(tags.add("pressure", "500"));
    const Predicate p = level_at_least(LogLevel::Warn) && tag_less("pressure", 1000);

    EXPECT_TRUE(p.evaluate(tags, LogLevel::Error));
    EXPECT_FALSE(p.evaluate(tags, LogLevel::Info));
}

// Missing or unusable tags.

TEST(PredicateTest, TextComparisonsAgainstMissingTagAreFalse) {
    const auto tags = make_tags();
    EXPECT_FALSE(tag_equals("k", "").evaluate(tags, LogLevel::Info));
    EXPECT_FALSE(tag_equals("k", "").evaluate(tags));
    EXPECT_FALSE(tag_equals("k", "x").evaluate(tags, LogLevel::Info));
    EXPECT_FALSE(tag_not_equals("k", "").evaluate(tags, LogLevel::Info));
    EXPECT_FALSE(tag_not_equals("k", "").evaluate(tags));
    EXPECT_FALSE(tag_not_equals("k", "x").evaluate(tags, LogLevel::Info));
}

TEST(PredicateTest, NumericComparisonsAgainstMissingOrNonNumericTagAreFalse) {
    TagSet<16> tags;
    EXPECT_TRUE(tags.add("name", "pump"));
    EXPECT_TRUE(tags.add("partial", "42abc"));
    EXPECT_TRUE(tags.add("empty", ""));
    EXPECT_TRUE(tags.add("signed", "+1"));
    EXPECT_TRUE(tags.add("spaced", " 1"));

    for (const std::string_view key : {"name", "partial", "empty", "signed", "spaced", "missing"}) {
        for (const CompareOp op : all_ops) {
            for (const double value : {0.0, 1.0}) {
                EXPECT_FALSE(tag_compare(key, op, value).evaluate(tags, LogLevel::Info))
                    << "key=" << key << " op=" << static_cast<int32_t>(op) << " value=" << value;
                EXPECT_FALSE(tag_compare(key, op, value).evaluate(tags))
                    << "key=" << key << " op=" << static_cast<int32_t>(op) << " value=" << value;
            }
        }
    }
}

TEST(PredicateTest, NegatedComparisonOfMissingTagIsTrue) {
    // `!` is logical negation, not a comparison, so it does not follow the
    // missing-tag rule.
    const auto tags = make_tags();
    EXPECT_TRUE((!tag_equals("k", "")).evaluate(tags, LogLevel::Info));
    EXPECT_TRUE((!tag_compare("k", CompareOp::NotEqual, 1.0)).evaluate(tags, LogLevel::Info));
}

// Duplicate keys.

TEST(PredicateTest, FirstOfDuplicateKeysDecidesTextComparisons) {
    TagSet<16> tags;
    EXPECT_TRUE(tags.add("k", "a"));
    EXPECT_TRUE(tags.add("k", "b"));
    ASSERT_EQ(tags.count(), 2U);

    EXPECT_TRUE(tag_exists("k").evaluate(tags, LogLevel::Info));
    EXPECT_TRUE(tag_equals("k", "a").evaluate(tags, LogLevel::Info));
    EXPECT_FALSE(tag_equals("k", "b").evaluate(tags, LogLevel::Info));
    EXPECT_FALSE(tag_not_equals("k", "a").evaluate(tags, LogLevel::Info));
    EXPECT_TRUE(tag_not_equals("k", "b").evaluate(tags, LogLevel::Info));
}

TEST(PredicateTest, FirstOfDuplicateKeysDecidesNumericComparisons) {
    TagSet<16> tags;
    EXPECT_TRUE(tags.add("n", "1"));
    EXPECT_TRUE(tags.add("n", "5"));
    EXPECT_TRUE(tags.add("m", "abc"));
    EXPECT_TRUE(tags.add("m", "5"));
    ASSERT_EQ(tags.count(), 4U);

    EXPECT_TRUE(tag_compare("n", CompareOp::Equal, 1.0).evaluate(tags, LogLevel::Info));
    EXPECT_FALSE(tag_compare("n", CompareOp::Equal, 5.0).evaluate(tags, LogLevel::Info));
    EXPECT_FALSE(tag_greater("n", 3.0).evaluate(tags, LogLevel::Info));
    // The first value is not a number; the later numeric one is not consulted.
    EXPECT_FALSE(tag_compare("m", CompareOp::Equal, 5.0).evaluate(tags, LogLevel::Info));
}

// And.

TEST(PredicateTest, AndBothTrue) {
    const Predicate p = tag_equals("env", "prod") && level_at_least(LogLevel::Info);
    auto tags = make_tags();

    EXPECT_TRUE(p.evaluate(tags, LogLevel::Warn));
}

TEST(PredicateTest, AndWithFalseLeftIsFalse) {
    const Predicate p = never() && level_at_least(LogLevel::Info);
    auto tags = make_tags();

    EXPECT_FALSE(p.evaluate(tags, LogLevel::Warn));
}

TEST(PredicateTest, AndRightFalse) {
    const Predicate p = level_at_least(LogLevel::Info) && tag_equals("env", "missing");
    auto tags = make_tags();

    EXPECT_FALSE(p.evaluate(tags, LogLevel::Warn));
}

TEST(PredicateTest, AlwaysIsNeutralInAnd) {
    auto tags = make_tags();
    EXPECT_TRUE((always() && tag_equals("env", "prod")).evaluate(tags, LogLevel::Trace));
    EXPECT_FALSE((always() && tag_equals("env", "dev")).evaluate(tags, LogLevel::Trace));
    EXPECT_TRUE((always() && always()).evaluate(tags, LogLevel::Trace));
}

// Or.

TEST(PredicateTest, OrWithTrueLeftIsTrue) {
    const Predicate p = always() || never();
    auto tags = make_tags();

    EXPECT_TRUE(p.evaluate(tags, LogLevel::Trace));
}

TEST(PredicateTest, OrRightTrue) {
    const Predicate p = never() || always();
    auto tags = make_tags();

    EXPECT_TRUE(p.evaluate(tags, LogLevel::Trace));
}

TEST(PredicateTest, OrBothFalse) {
    const Predicate p = never() || tag_equals("env", "missing");
    auto tags = make_tags();

    EXPECT_FALSE(p.evaluate(tags, LogLevel::Trace));
}

TEST(PredicateTest, NeverIsNeutralInOr) {
    auto tags = make_tags();
    EXPECT_TRUE((never() || tag_equals("env", "prod")).evaluate(tags, LogLevel::Trace));
    EXPECT_FALSE((never() || tag_equals("env", "dev")).evaluate(tags, LogLevel::Trace));
    EXPECT_FALSE((never() || never()).evaluate(tags, LogLevel::Trace));
}

// Not.

TEST(PredicateTest, NotInvertsAlways) {
    const Predicate p = !always();
    const TagSet<16> tags;

    EXPECT_FALSE(p.evaluate(tags, LogLevel::Info));
}

TEST(PredicateTest, NotInvertsNever) {
    const Predicate p = !never();
    const TagSet<16> tags;

    EXPECT_TRUE(p.evaluate(tags, LogLevel::Info));
}

TEST(PredicateTest, NotInvertsTagEquals) {
    const Predicate p = !tag_equals("env", "prod");
    auto tags = make_tags();

    EXPECT_FALSE(p.evaluate(tags, LogLevel::Info));
}

// Composite.

TEST(PredicateTest, CompositeEveryCombination) {
    auto tags = make_tags();

    // (env=prod || app=other) && level>=INFO && !tag_exists(missing)
    const Predicate p = (tag_equals("env", "prod") || tag_equals("app", "other")) &&
                        level_at_least(LogLevel::Info) && !tag_exists("missing");

    EXPECT_TRUE(p.evaluate(tags, LogLevel::Info));
    EXPECT_FALSE(p.evaluate(tags, LogLevel::Debug));
    EXPECT_TRUE(p.evaluate(tags, LogLevel::Error));

    // Missing tag condition fails
    const Predicate p2 =
        (tag_equals("env", "prod") || tag_equals("app", "other")) && tag_exists("missing");
    EXPECT_FALSE(p2.evaluate(tags, LogLevel::Info));
}

TEST(PredicateTest, DeepNesting) {
    auto tags = make_tags();

    // !((never() && always()) || (level_at_least(INFO) && !tag_equals("env", "prod")))
    const Predicate p =
        !((never() && always()) || (level_at_least(LogLevel::Info) && !tag_equals("env", "prod")));

    // Inner: never()&&always()=false, so || depends on right side
    // Right: level>=INFO(true for INFO) && !env=prod(false for "prod") = false
    // Or: false || false = false
    // Not: true
    EXPECT_TRUE(p.evaluate(tags, LogLevel::Info));
}

// Default-constructed predicates.

TEST(PredicateTest, DefaultPredicateMatchesEveryRecord) {
    const Predicate p;
    const TagSet<16> tags;
    EXPECT_TRUE(p.evaluate(tags, LogLevel::Trace));
    EXPECT_TRUE(p.evaluate(tags, LogLevel::Fatal));
    EXPECT_TRUE(p.evaluate(tags));
}

TEST(PredicateTest, DefaultPredicateActsAsAlwaysWhenCombined) {
    const Predicate unset;
    const auto tags = make_tags();
    EXPECT_FALSE((unset && never()).evaluate(tags, LogLevel::Info));
    EXPECT_TRUE((unset && tag_equals("env", "prod")).evaluate(tags, LogLevel::Info));
    EXPECT_TRUE((never() || unset).evaluate(tags, LogLevel::Info));
    EXPECT_FALSE((!unset).evaluate(tags, LogLevel::Info));
}

TEST(PredicateTest, TagConditionsAreFalseOnEmptyTagSet) {
    const Predicate p = tag_equals("env", "prod") && level_at_least(LogLevel::Info);
    const TagSet<16> empty;

    EXPECT_FALSE(p.evaluate(empty, LogLevel::Info));
    EXPECT_FALSE(p.evaluate(empty, LogLevel::Warn));
}

// Copy semantics.

TEST(PredicateTest, CopyPreservesBehavior) {
    const Predicate p1 = tag_equals("env", "prod") && level_at_least(LogLevel::Info);
    // The copies are what this test is about.
    // NOLINTBEGIN(performance-unnecessary-copy-initialization)
    const Predicate p2(p1);
    const Predicate p3 = p1;
    // NOLINTEND(performance-unnecessary-copy-initialization)

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
    const Predicate p3 = std::move(p2);

    auto tags = make_tags();
    EXPECT_TRUE(p3.evaluate(tags, LogLevel::Info));
}

TEST(PredicateTest, AssignmentPreservesBehavior) {
    const Predicate p1 = tag_equals("env", "prod");
    Predicate p2 = never();
    p2 = p1;

    auto tags = make_tags();
    EXPECT_TRUE(p2.evaluate(tags, LogLevel::Info));

    const TagSet<16> empty;
    EXPECT_FALSE(p2.evaluate(empty, LogLevel::Info));
}

TEST(PredicateTest, PredicatesSharingNodesWorkAfterOriginalIsDestroyed) {
    auto original =
        std::make_unique<Predicate>(tag_equals("env", "prod") && level_at_least(LogLevel::Info));
    const Predicate copy = *original;
    Predicate assigned;
    assigned = *original;
    Predicate moved_into = Predicate(*original);
    const Predicate moved = std::move(moved_into);
    const Predicate combined = *original || tag_equals("env", "dev");
    const Predicate negated = !*original;
    original.reset();

    const auto prod = make_tags();
    for (const Predicate* p :
         std::array<const Predicate*, 4>{&copy, &assigned, &moved, &combined}) {
        EXPECT_TRUE(p->evaluate(prod, LogLevel::Warn));
        EXPECT_FALSE(p->evaluate(prod, LogLevel::Debug));
    }
    EXPECT_FALSE(negated.evaluate(prod, LogLevel::Warn));
    EXPECT_TRUE(negated.evaluate(prod, LogLevel::Debug));
}

// Deep trees built in C++.
//
// The parser limits nesting, the operators do not. Building, evaluating and
// destroying these must neither copy the tree on each step nor recurse once
// per level.

inline constexpr int32_t deep_terms = 100'000;

TEST(PredicateTest, LeftDeepAndChainOf100000TermsEvaluates) {
    Predicate chain = always();
    for (int32_t i = 0; i < deep_terms; ++i) {
        chain = chain && tag_exists("k");
    }

    TagSet<16> with_k;
    EXPECT_TRUE(with_k.add("k", "1"));
    EXPECT_TRUE(chain.evaluate(with_k, LogLevel::Info));
    EXPECT_TRUE(chain.evaluate(with_k));
    EXPECT_FALSE(chain.evaluate(make_tags(), LogLevel::Info));
    EXPECT_FALSE(chain.evaluate(make_tags()));
}

TEST(PredicateTest, RightDeepOrChainOf100000TermsEvaluates) {
    Predicate chain = never();
    for (int32_t i = 0; i < deep_terms; ++i) {
        chain = tag_exists("k") || chain;
    }

    TagSet<16> with_k;
    EXPECT_TRUE(with_k.add("k", "1"));
    EXPECT_TRUE(chain.evaluate(with_k, LogLevel::Info));
    // Without `k` every one of the terms is looked at, down to the bottom.
    EXPECT_FALSE(chain.evaluate(make_tags(), LogLevel::Info));
}

TEST(PredicateTest, ChainOf100000NegationsEvaluates) {
    Predicate chain = tag_exists("env");
    for (int32_t i = 0; i < deep_terms; ++i) {
        chain = !chain;  // an even count
    }

    EXPECT_TRUE(chain.evaluate(make_tags(), LogLevel::Info));
    EXPECT_FALSE(chain.evaluate(TagSet<16>{}, LogLevel::Info));
}

TEST(PredicateTest, DeepChainsSharingNodesCanBeDestroyedOnTwoThreadsAtOnce) {
    Predicate shared = always();
    for (int32_t i = 0; i < deep_terms; ++i) {
        shared = shared && tag_exists("k");
    }
    TagSet<16> with_k;
    EXPECT_TRUE(with_k.add("k", "1"));

    // Each thread grows its own chain on top of the shared one, evaluates it
    // and destroys its own nodes while the other thread may be doing the same.
    const auto grow_evaluate_drop = [&shared, &with_k](bool& result) {
        Predicate own = shared;
        for (int32_t i = 0; i < deep_terms; ++i) {
            own = own && tag_exists("k");
        }
        result = own.evaluate(with_k, LogLevel::Info);
    };
    bool first = false;
    bool second = false;
    std::thread a(grow_evaluate_drop, std::ref(first));
    std::thread b(grow_evaluate_drop, std::ref(second));
    a.join();
    b.join();

    EXPECT_TRUE(first);
    EXPECT_TRUE(second);
    EXPECT_TRUE(shared.evaluate(with_k, LogLevel::Info));
}

// Records without a level (metrics, progress).

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
    EXPECT_TRUE(match.add("run_id", "42"));
    TagSet<16> other;
    EXPECT_TRUE(other.add("run_id", "7"));
    const Predicate p =
        level_at_least(LogLevel::Warn) && tag_compare("run_id", CompareOp::Equal, 42);

    EXPECT_TRUE(p.evaluate(match));
    EXPECT_FALSE(p.evaluate(other));
}

TEST(PredicateTest, LevelConditionIsIgnoredInsideOr) {
    // Treating the level part as "true" would pass every metric here.
    TagSet<16> other;
    EXPECT_TRUE(other.add("run_id", "7"));
    const Predicate p =
        level_at_least(LogLevel::Warn) || tag_compare("run_id", CompareOp::Equal, 42);

    EXPECT_FALSE(p.evaluate(other));
}

TEST(PredicateTest, NegatedLevelConditionIsIgnoredForRecordWithoutLevel) {
    const Predicate negated = !level_at_least(LogLevel::Warn);
    TagSet<16> with_x;
    EXPECT_TRUE(with_x.add("x", "1"));
    const TagSet<16> without_x;

    // Fails if the negated level is taken as false: the tag alone must decide.
    EXPECT_TRUE((negated && tag_exists("x")).evaluate(with_x));
    // Fails if the negated level is taken as true: it would pass the record.
    EXPECT_FALSE((negated || tag_exists("x")).evaluate(without_x));
    // Either way, the right operand decides from either side.
    EXPECT_TRUE((tag_exists("x") && negated).evaluate(with_x));
    EXPECT_FALSE((tag_exists("x") || negated).evaluate(without_x));
}

TEST(PredicateTest, TagOnlyPredicatesBehaveTheSameWithOrWithoutLevel) {
    TagSet<16> tags;
    EXPECT_TRUE(tags.add("env", "prod"));
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
