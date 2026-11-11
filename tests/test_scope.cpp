// LUMEN_SCOPE and LUMEN_LOOP: the tags they add, the progress LUMEN_LOOP
// sends, and the nesting rule.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "lumen/macros.hpp"
#include "test_support.hpp"

namespace lumen {
namespace {

using test::GlobalCoreTest;
using test::LocalCoreTest;
using test::log_to;
using test::tag_of;
using test::tags_of;

using States = std::vector<std::pair<uint64_t, uint64_t>>;

// LUMEN_SCOPE.

class ScopeTest : public LocalCoreTest {
protected:
    [[nodiscard]] std::vector<LogRecord> _logs() {
        _core.flush();
        return _captured->logs();
    }
};

TEST_F(ScopeTest, ScopeTagIsOnRecordsInsideTheBlockAndAbsentAfterIt) {
    log_to(_core, "before");
    {
        LUMEN_SCOPE("request", "42");
        log_to(_core, "inside");
    }
    log_to(_core, "after");
    const auto records = _logs();
    ASSERT_EQ(records.size(), 3U);
    EXPECT_EQ(tag_of(records[0].tags, "request"), std::nullopt);
    EXPECT_EQ(tag_of(records[1].tags, "request"), "42");
    EXPECT_EQ(tag_of(records[2].tags, "request"), std::nullopt);
}

TEST_F(ScopeTest, ScopeValueFromATemporaryStringIsCopied) {
    {
        LUMEN_SCOPE(std::string("run"), std::to_string(1234567890123LL) + std::string(40, 'x'));
        log_to(_core, "inside");
    }
    const auto records = _logs();
    ASSERT_EQ(records.size(), 1U);
    EXPECT_EQ(tag_of(records[0].tags, "run"), "1234567890123" + std::string(40, 'x'));
}

TEST_F(ScopeTest, NestedScopesAllContributeAndTheInnerValueWinsForASharedKey) {
    {
        LUMEN_SCOPE("outer", "1");
        LUMEN_SCOPE("shared", "outer");
        {
            LUMEN_SCOPE("inner", "2");
            LUMEN_SCOPE("shared", "inner");
            log_to(_core, "nested");
        }
        log_to(_core, "outer only");
    }
    const auto records = _logs();
    ASSERT_EQ(records.size(), 2U);
    EXPECT_EQ(tags_of(records[0].tags), (std::vector<std::pair<std::string, std::string>>{
                                            {"shared", "inner"}, {"inner", "2"}, {"outer", "1"}}));
    EXPECT_EQ(tags_of(records[1].tags), (std::vector<std::pair<std::string, std::string>>{
                                            {"shared", "outer"}, {"outer", "1"}}));
}

TEST_F(ScopeTest, TwentyNestedScopesFillTheRecordInnermostFirstAndCountTheRest) {
    std::vector<std::unique_ptr<detail::ScopeGuard>> guards;
    guards.reserve(20);
    for (int i = 0; i < 20; ++i) {
        guards.push_back(
            std::make_unique<detail::ScopeGuard>("s" + std::to_string(i), std::to_string(i)));
    }
    log_to(_core, "deep");
    while (!guards.empty()) {
        guards.pop_back();  // innermost first
    }
    const auto records = _logs();
    ASSERT_EQ(records.size(), 1U);
    const auto tags = tags_of(records[0].tags);
    ASSERT_EQ(tags.size(), max_record_tags);
    EXPECT_EQ(tags.front().first, "s19");
    EXPECT_EQ(tags.back().first, "s4");
    EXPECT_EQ(_core.statistics().tags_dropped, 4U);
}

TEST_F(ScopeTest, ScopeTagIsPresentWhenTheExceptionIsThrownAndGoneAfterUnwinding) {
    try {
        LUMEN_SCOPE("phase", "risky");
        log_to(_core, "before throw");
        throw std::runtime_error("unwind");
    } catch (const std::runtime_error& e) {
        EXPECT_STREQ(e.what(), "unwind");
        log_to(_core, "in handler");
    }
    const auto records = _logs();
    ASSERT_EQ(records.size(), 2U);
    EXPECT_EQ(tag_of(records[0].tags, "phase"), "risky");
    EXPECT_EQ(tag_of(records[1].tags, "phase"), std::nullopt);
}

TEST_F(ScopeTest, ScopeOnOneThreadDoesNotReachRecordsOfAnother) {
    LUMEN_SCOPE("owner", "main");
    std::thread other([this] { log_to(_core, "other"); });
    other.join();
    const auto records = _logs();
    ASSERT_EQ(records.size(), 1U);
    EXPECT_EQ(tag_of(records[0].tags, "owner"), std::nullopt);
}

#ifndef NDEBUG
TEST(ScopeDeathTest, DestroyingScopeGuardsOutOfOrderAssertsInDebugBuilds) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            auto first = std::make_unique<detail::ScopeGuard>("a", "1");
            auto second = std::make_unique<detail::ScopeGuard>("b", "2");
            first.reset();
        },
        "out of order");
}

TEST(ScopeDeathTest, DestroyingAScopeGuardOnAnotherThreadAssertsInDebugBuilds) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            auto guard = std::make_unique<detail::ScopeGuard>("a", "1");
            std::thread other([&guard] { guard.reset(); });
            other.join();
        },
        "on a thread other than its own");
}
#endif

// LUMEN_LOOP (it drives a progress bar on the global core).

class LoopTest : public GlobalCoreTest {
protected:
    // (current, total) of the progress records with `label`.
    [[nodiscard]] States _states(std::string_view label) {
        core().flush();
        States out;
        for (const ProgressRecord& r : _captured->progress()) {
            if (r.label == label) {
                out.emplace_back(r.current, r.total);
            }
        }
        return out;
    }
};

TEST_F(LoopTest, CompletedLoopSendsZeroThenOneProgressRecordPerCompletedIteration) {
    std::vector<int> seen;
    LUMEN_LOOP("L", i, 3) {
        seen.push_back(i);
    }
    EXPECT_EQ(seen, (std::vector<int>{0, 1, 2}));
    EXPECT_EQ(_states("L"), (States{{0, 3}, {1, 3}, {2, 3}, {3, 3}}));
}

TEST_F(LoopTest, ProgressBeforeABodyCountsOnlyTheBodiesAlreadyCompleted) {
    std::vector<States> seen_at_body_start;
    LUMEN_LOOP("L", i, 3) {
        static_cast<void>(i);
        seen_at_body_start.push_back(_states("L"));
    }
    ASSERT_EQ(seen_at_body_start.size(), 3U);
    EXPECT_EQ(seen_at_body_start[2].back(), (std::pair<uint64_t, uint64_t>{2, 3}));
}

TEST_F(LoopTest, BreakDoesNotClaimCompletion) {
    LUMEN_LOOP("L", i, 100) {
        if (i == 1) {
            break;
        }
    }
    EXPECT_EQ(_states("L"), (States{{0, 100}, {1, 100}}));
}

TEST_F(LoopTest, ContinueCountsTheIterationAsCompleted) {
    int bodies_finished = 0;
    LUMEN_LOOP("L", i, 3) {
        if (i == 1) {
            continue;
        }
        ++bodies_finished;
    }
    EXPECT_EQ(bodies_finished, 2);
    EXPECT_EQ(_states("L"), (States{{0, 3}, {1, 3}, {2, 3}, {3, 3}}));
}

TEST_F(LoopTest, ExceptionLeavesTheLastCompletedCountAndNoCompletion) {
    try {
        LUMEN_LOOP("L", i, 5) {
            if (i == 2) {
                throw std::runtime_error("stop");
            }
        }
    } catch (const std::runtime_error& e) {
        EXPECT_STREQ(e.what(), "stop");
    }
    EXPECT_EQ(_states("L"), (States{{0, 5}, {1, 5}, {2, 5}}));
}

TEST_F(LoopTest, ZeroIterationsSendsTheSingleRecordZeroOfZeroAndRunsNoBody) {
    int bodies = 0;
    LUMEN_LOOP("L", i, 0) {
        static_cast<void>(i);
        ++bodies;
    }
    EXPECT_EQ(bodies, 0);
    EXPECT_EQ(_states("L"), (States{{0, 0}}));
}

TEST_F(LoopTest, NegativeTotalRunsNoBodyLikeZero) {
    int bodies = 0;
    LUMEN_LOOP("L", i, -3) {
        static_cast<void>(i);
        ++bodies;
    }
    EXPECT_EQ(bodies, 0);
    EXPECT_EQ(_states("L"), (States{{0, 0}}));
}

TEST_F(LoopTest, LoopVariableHasTheTypeOfTheTotalAndIsConst) {
    const std::size_t total = 2;
    LUMEN_LOOP("L", i, total) {
        static_assert(std::is_same_v<decltype(i), const std::size_t>);
        static_cast<void>(i);
    }
    LUMEN_LOOP("M", j, int64_t{2}) {
        static_assert(std::is_same_v<decltype(j), const int64_t>);
        static_cast<void>(j);
    }
}

TEST_F(LoopTest, BodyRecordsCarryTheIterationIndexUnderTheLoopLabel) {
    LUMEN_LOOP("epoch", e, 2) {
        LUMEN_LOG_INFO("body {}", e);
    }
    LUMEN_LOG_INFO("after");
    core().flush();
    const auto records = _captured->logs();
    ASSERT_EQ(records.size(), 3U);
    EXPECT_EQ(tag_of(records[0].tags, "epoch"), "0");
    EXPECT_EQ(tag_of(records[1].tags, "epoch"), "1");
    EXPECT_EQ(tag_of(records[2].tags, "epoch"), std::nullopt);
}

TEST_F(LoopTest, NestedLoopsEachKeepTheirOwnIterationTagAndBar) {
    LUMEN_LOOP("outer", o, 2) {
        LUMEN_LOOP("inner", n, 2) {
            LUMEN_LOG_INFO("cell {} {}", o, n);
        }
    }
    core().flush();
    const auto records = _captured->logs();
    ASSERT_EQ(records.size(), 4U);
    EXPECT_EQ(tag_of(records[3].tags, "outer"), "1");
    EXPECT_EQ(tag_of(records[3].tags, "inner"), "1");
    EXPECT_EQ(tag_of(records[1].tags, "outer"), "0");
    EXPECT_EQ(tag_of(records[1].tags, "inner"), "1");
    EXPECT_EQ(_states("outer"), (States{{0, 2}, {1, 2}, {2, 2}}));
    // Two inner bars (different ids), each complete.
    EXPECT_EQ(_states("inner"), (States{{0, 2}, {1, 2}, {2, 2}, {0, 2}, {1, 2}, {2, 2}}));
}

// The missing braces are what this test is about.
// NOLINTBEGIN(readability-braces-around-statements)
TEST_F(LoopTest, ElseAfterALoopAsTheBodyOfAnIfBindsToThatIf) {
    int else_taken = 0;
    int bodies = 0;
    const bool run = false;
    if (run) LUMEN_LOOP("L", i, 2) {
            static_cast<void>(i);
            ++bodies;
        }
    else
        ++else_taken;
    EXPECT_EQ(bodies, 0);
    EXPECT_EQ(else_taken, 1);
}
// NOLINTEND(readability-braces-around-statements)

TEST_F(LoopTest, LabelAndTotalAreEvaluatedOnce) {
    int label_calls = 0;
    int total_calls = 0;
    auto label = [&] {
        ++label_calls;
        return std::string("L");
    };
    auto total = [&] {
        ++total_calls;
        return 3;
    };
    LUMEN_LOOP(label(), i, total()) {
        static_cast<void>(i);
    }
    EXPECT_EQ(label_calls, 1);
    EXPECT_EQ(total_calls, 1);
}

}  // namespace
}  // namespace lumen
