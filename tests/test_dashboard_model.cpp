// DashboardModel: scrollback, metric history, and the progress bar table
// with its ordering and eviction rules.
#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "lumen/dashboard_model.hpp"

namespace lumen {
namespace {

constexpr uint64_t second_ns = 1'000'000'000;

[[nodiscard]] ProgressRecord bar_state(uint64_t id, uint64_t current, uint64_t total,
                                       uint64_t at_ns, std::string_view label = "bar") {
    ProgressRecord record;
    record.id = id;
    record.label = label;
    record.current = current;
    record.total = total;
    record.timestamp_ns = at_ns;
    return record;
}

[[nodiscard]] std::vector<uint64_t> bar_ids(const DashboardModel& model) {
    std::vector<uint64_t> ids;
    for (const DashboardModel::ProgressBar& bar : model.progress_bars()) {
        ids.push_back(bar.id);
    }
    return ids;
}

[[nodiscard]] std::vector<std::string> texts(const DashboardModel& model) {
    std::vector<std::string> out;
    for (const DashboardModel::LogLine& line : model.scrollback()) {
        out.push_back(line.text);
    }
    return out;
}

using Ids = std::vector<uint64_t>;

TEST(DashboardModelTest, ScrollbackKeepsTheNewestLinesWithTheirLevels) {
    DashboardModel model({.scrollback_lines = 2}, {});
    model.add_log(LogLevel::Info, "one");
    model.add_log(LogLevel::Warn, "two");
    model.add_log(LogLevel::Error, "three");
    EXPECT_EQ(texts(model), (std::vector<std::string>{"two", "three"}));
    ASSERT_EQ(model.scrollback().size(), 2U);
    EXPECT_EQ(model.scrollback().at(0).level, LogLevel::Warn);
    EXPECT_EQ(model.scrollback().at(1).level, LogLevel::Error);
}

TEST(DashboardModelTest, NamedMetricsKeepTheirOrderAndOthersAreIgnored) {
    DashboardModel model({}, {"loss", "accuracy"});
    model.add_metric("accuracy", 0.9);
    model.add_metric("unrelated", 1.0);
    model.add_metric("loss", 0.5);
    ASSERT_EQ(model.metrics().size(), 2U);
    EXPECT_EQ(model.metrics().at(0).name, "loss");
    EXPECT_EQ(model.metrics().at(0).values, (std::deque<double>{0.5}));
    EXPECT_EQ(model.metrics().at(1).name, "accuracy");
    EXPECT_EQ(model.metrics().at(1).values, (std::deque<double>{0.9}));
}

TEST(DashboardModelTest, WithoutNamesTheFirstMaxMetricsNamesSeenAreTrackedInThatOrder) {
    DashboardModel model({.max_metrics = 2}, {});
    model.add_metric("b", 1.0);
    model.add_metric("a", 2.0);
    model.add_metric("c", 3.0);
    model.add_metric("b", 4.0);
    ASSERT_EQ(model.metrics().size(), 2U);
    EXPECT_EQ(model.metrics().at(0).name, "b");
    EXPECT_EQ(model.metrics().at(0).values, (std::deque<double>{1.0, 4.0}));
    EXPECT_EQ(model.metrics().at(1).name, "a");
}

TEST(DashboardModelTest, MetricHistoryDropsTheOldestValueAndRangeFollowsTheWindow) {
    DashboardModel model({.metric_history = 3}, {"m"});
    for (const double value : {10.0, 1.0, 5.0, 7.0}) {
        model.add_metric("m", value);
    }
    ASSERT_EQ(model.metrics().size(), 1U);
    const DashboardModel::MetricSeries& series = model.metrics().front();
    EXPECT_EQ(series.values, (std::deque<double>{1.0, 5.0, 7.0}));
    EXPECT_EQ(series.min, 1.0);
    EXPECT_EQ(series.max, 7.0);  // 10 has left the window
}

TEST(DashboardModelTest, RangeIgnoresNonFiniteValuesAndIsNaNWhenNoneIsFinite) {
    DashboardModel model({}, {"m"});
    model.add_metric("m", std::numeric_limits<double>::quiet_NaN());
    ASSERT_EQ(model.metrics().size(), 1U);
    EXPECT_TRUE(std::isnan(model.metrics().front().min));
    EXPECT_TRUE(std::isnan(model.metrics().front().max));
    model.add_metric("m", std::numeric_limits<double>::infinity());
    model.add_metric("m", 2.0);
    EXPECT_EQ(model.metrics().front().min, 2.0);
    EXPECT_EQ(model.metrics().front().max, 2.0);
}

TEST(DashboardModelTest, BarsKeepTheOrderInWhichTheyFirstAppearedWhenUpdated) {
    DashboardModel model({}, {});
    model.add_progress(bar_state(30, 1, 10, 0));
    model.add_progress(bar_state(10, 1, 10, 0));
    model.add_progress(bar_state(20, 1, 10, 0));
    model.add_progress(bar_state(10, 5, 10, 1));
    model.add_progress(bar_state(30, 6, 10, 2));
    EXPECT_EQ(bar_ids(model), (Ids{30, 10, 20}));
    ASSERT_EQ(model.progress_bars().size(), 3U);
    EXPECT_EQ(model.progress_bars().at(1).current, 5U);
    EXPECT_EQ(model.progress_bars().at(0).current, 6U);
}

TEST(DashboardModelTest, FinishedBarStaysForTheLingerTimeThenIsRemoved) {
    DashboardModel model({.finished_bar_linger = std::chrono::seconds{2}}, {});
    model.add_progress(bar_state(1, 10, 10, 5 * second_ns));
    model.expire_progress(6 * second_ns);
    EXPECT_EQ(bar_ids(model), (Ids{1}));
    EXPECT_TRUE(model.progress_bars().front().finished());
    model.expire_progress(7 * second_ns);
    EXPECT_TRUE(model.progress_bars().empty());
}

TEST(DashboardModelTest, UnfinishedBarIsRemovedOnlyAfterTheIdleTimeout) {
    DashboardModel model({.finished_bar_linger = std::chrono::seconds{1},
                          .idle_bar_timeout = std::chrono::seconds{10}},
                         {});
    model.add_progress(bar_state(1, 3, 10, 0));
    model.expire_progress(9 * second_ns);
    EXPECT_EQ(bar_ids(model), (Ids{1}));
    model.expire_progress(10 * second_ns);
    EXPECT_TRUE(model.progress_bars().empty());
}

TEST(DashboardModelTest, ZeroIdleTimeoutKeepsAnUnfinishedBarForever) {
    DashboardModel model({.idle_bar_timeout = std::chrono::nanoseconds{0}}, {});
    model.add_progress(bar_state(1, 3, 10, 0));
    model.expire_progress(std::numeric_limits<uint64_t>::max());
    EXPECT_EQ(bar_ids(model), (Ids{1}));
}

TEST(DashboardModelTest, BarWithZeroTotalIsNeverFinishedAndStaysUntilIdle) {
    DashboardModel model({.finished_bar_linger = std::chrono::seconds{1},
                          .idle_bar_timeout = std::chrono::seconds{10}},
                         {});
    model.add_progress(bar_state(1, 0, 0, 0));
    model.add_progress(bar_state(1, 42, 0, 0));
    EXPECT_FALSE(model.progress_bars().front().finished());
    model.expire_progress(5 * second_ns);
    EXPECT_EQ(bar_ids(model), (Ids{1}));
    model.expire_progress(10 * second_ns);
    EXPECT_TRUE(model.progress_bars().empty());
}

TEST(DashboardModelTest, NewBarBeyondTheLimitEvictsTheOldestFinishedBarFirst) {
    DashboardModel model({.max_progress_bars = 3}, {});
    model.add_progress(bar_state(1, 1, 10, 100));   // oldest, unfinished
    model.add_progress(bar_state(2, 10, 10, 300));  // finished, newer
    model.add_progress(bar_state(3, 10, 10, 200));  // finished, older
    model.add_progress(bar_state(4, 1, 10, 400));
    EXPECT_EQ(bar_ids(model), (Ids{1, 2, 4}));
}

TEST(DashboardModelTest, WithoutFinishedBarsANewBarEvictsTheLeastRecentlyUpdated) {
    DashboardModel model({.max_progress_bars = 2}, {});
    model.add_progress(bar_state(1, 1, 10, 100));
    model.add_progress(bar_state(2, 1, 10, 200));
    model.add_progress(bar_state(1, 2, 10, 300));  // bar 1 is now the more recent
    model.add_progress(bar_state(3, 1, 10, 400));
    EXPECT_EQ(bar_ids(model), (Ids{1, 3}));
}

TEST(DashboardModelTest, ZeroCountLimitsAreTakenAsOne) {
    DashboardModel model({.scrollback_lines = 0, .metric_history = 0, .max_progress_bars = 0},
                         {"m"});
    model.add_log(LogLevel::Info, "a");
    model.add_log(LogLevel::Info, "b");
    model.add_metric("m", 1.0);
    model.add_metric("m", 2.0);
    model.add_progress(bar_state(1, 1, 10, 0));
    model.add_progress(bar_state(2, 1, 10, 1));
    EXPECT_EQ(texts(model), (std::vector<std::string>{"b"}));
    EXPECT_EQ(model.metrics().front().values, (std::deque<double>{2.0}));
    EXPECT_EQ(bar_ids(model), (Ids{2}));
}

}  // namespace
}  // namespace lumen
