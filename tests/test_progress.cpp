// ProgressHandle: which states reach the sink.
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "lumen/progress.hpp"
#include "test_support.hpp"

namespace lumen {
namespace {

using test::LocalCoreTest;

class ProgressTest : public LocalCoreTest {
protected:
    // (current, total) of every progress record delivered so far.
    [[nodiscard]] std::vector<std::pair<uint64_t, uint64_t>> states() {
        core_.flush();
        std::vector<std::pair<uint64_t, uint64_t>> out;
        for (const ProgressRecord& r : captured_->progress())
            out.emplace_back(r.current, r.total);
        return out;
    }
    using States = std::vector<std::pair<uint64_t, uint64_t>>;
};

TEST_F(ProgressTest, CreatingAHandleSendsNothing) {
    const ProgressHandle bar(core_, "idle", 10);
    EXPECT_TRUE(states().empty());
}

TEST_F(ProgressTest, UpdateTickAndFinishSendEachNewStateOnce) {
    ProgressHandle bar(core_, "work", 4);
    bar.update(1);
    bar.tick();
    bar.update(2);  // same state as the last one sent: not sent again
    bar.finish();
    EXPECT_EQ(states(), (States{{1, 4}, {2, 4}, {4, 4}}));
}

TEST_F(ProgressTest, FinishAfterReachingTotalDoesNotSendTotalTwice) {
    ProgressHandle bar(core_, "work", 2);
    bar.update(2);
    bar.finish();
    EXPECT_EQ(states(), (States{{2, 2}}));
}

TEST_F(ProgressTest, CallsAfterFinishSendNothing) {
    ProgressHandle bar(core_, "work", 3);
    bar.finish();
    bar.update(1);
    bar.tick();
    bar.finish();
    EXPECT_EQ(states(), (States{{3, 3}}));
    EXPECT_TRUE(bar.finished());
}

TEST_F(ProgressTest, DestroyingAnUnfinishedHandleSendsNothingMore) {
    {
        ProgressHandle bar(core_, "abandoned", 1000);
        bar.tick();
    }
    EXPECT_EQ(states(), (States{{1, 1000}}));
}

TEST_F(ProgressTest, MovedFromHandleSendsNothingAndTheTargetContinuesTheSameBar) {
    ProgressHandle first(core_, "moved", 3);
    first.update(1);
    ProgressHandle second(std::move(first));
    first.update(2);  // NOLINT(bugprone-use-after-move): a moved-from handle must do nothing
    second.update(2);
    second.finish();
    core_.flush();
    const auto records = captured_->progress();
    ASSERT_EQ(records.size(), 3U);
    EXPECT_EQ(records[0].id, records[2].id);
    EXPECT_EQ(states(), (States{{1, 3}, {2, 3}, {3, 3}}));
}

TEST_F(ProgressTest, TwoHandlesHaveDifferentIdsAndTheirOwnLabels) {
    ProgressHandle a(core_, "a", 1);
    ProgressHandle b(core_, std::string(40, 'b'), 1);
    a.finish();
    b.finish();
    core_.flush();
    const auto records = captured_->progress();
    ASSERT_EQ(records.size(), 2U);
    EXPECT_NE(records[0].id, records[1].id);
    EXPECT_EQ(records[0].label, "a");
    EXPECT_EQ(records[1].label, std::string(40, 'b'));
}

}  // namespace
}  // namespace lumen
