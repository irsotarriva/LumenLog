// lumen/bridges/openmp.hpp against the real OpenMP runtime: the exact
// metrics and tags that reach a sink.
#include <gtest/gtest.h>

#include <omp.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "lumen/bridges/openmp.hpp"
#include "test_support.hpp"

namespace lumen {
namespace {

using bridges::OpenMpWorkBalance;
using test::tags_of;
using Tags = std::vector<std::pair<std::string, std::string>>;

// Restores the runtime settings a test changes.
class RuntimeSettings {
public:
    RuntimeSettings()
        : num_threads_(omp_get_max_threads())
        , dynamic_(omp_get_dynamic())
        , max_active_levels_(omp_get_max_active_levels()) {
        // A fixed team size: with dynamic adjustment the runtime may give a
        // num_threads(4) region fewer threads.
        omp_set_dynamic(0);
    }
    ~RuntimeSettings() {
        omp_set_num_threads(num_threads_);
        omp_set_dynamic(dynamic_);
        omp_set_max_active_levels(max_active_levels_);
    }
    RuntimeSettings(const RuntimeSettings&) = delete;
    RuntimeSettings& operator=(const RuntimeSettings&) = delete;
    RuntimeSettings(RuntimeSettings&&) = delete;
    RuntimeSettings& operator=(RuntimeSettings&&) = delete;

private:
    int32_t num_threads_;
    int32_t dynamic_;
    int32_t max_active_levels_;
};

class OpenMpBridgeTest : public test::LocalCoreTest {
protected:
    [[nodiscard]] std::vector<MetricRecord> _metrics() {
        _core.flush();
        return _captured->metrics();
    }

private:
    RuntimeSettings settings_;
};

TEST_F(OpenMpBridgeTest, ConfigurationEmitsTheRuntimesMaxThreadsAndNumProcsWithoutTags) {
    omp_set_num_threads(3);
    bridges::emit_openmp_configuration(_core);

    const auto metrics = _metrics();
    ASSERT_EQ(metrics.size(), 2U);
    EXPECT_EQ(metrics[0].name, "openmp.max_threads");
    EXPECT_EQ(metrics[0].value, 3.0);
    EXPECT_EQ(metrics[0].tags.count(), 0U);
    EXPECT_EQ(metrics[1].name, "openmp.num_procs");
    EXPECT_EQ(metrics[1].value, static_cast<double>(omp_get_num_procs()));
    EXPECT_EQ(metrics[1].tags.count(), 0U);
}

TEST_F(OpenMpBridgeTest, EvenWorkOverATeamOfFourGivesItsTotalAndAnImbalanceOfOne) {
    OpenMpWorkBalance balance("events", 4);
#pragma omp parallel num_threads(4)
    { balance.add(10); }
    balance.emit(_core);

    const auto metrics = _metrics();
    ASSERT_EQ(metrics.size(), 2U);
    const Tags expected_tags = {{"region", "events"}, {"threads", "4"}};
    EXPECT_EQ(metrics[0].name, "openmp.work.total");
    EXPECT_EQ(metrics[0].value, 40.0);
    EXPECT_EQ(tags_of(metrics[0].tags), expected_tags);
    EXPECT_EQ(metrics[1].name, "openmp.work.imbalance");
    EXPECT_DOUBLE_EQ(metrics[1].value, 1.0);
    EXPECT_EQ(tags_of(metrics[1].tags), expected_tags);
}

TEST_F(OpenMpBridgeTest, ImbalanceIsTheBusiestThreadOverTheMeanPerThread) {
    OpenMpWorkBalance balance("uneven", 4);
#pragma omp parallel num_threads(4)
    {
        // Threads 0..3 do 1, 2, 3 and 4 units: mean 2.5, busiest 4.
        const uint64_t units = static_cast<uint64_t>(omp_get_thread_num()) + 1;
        balance.add(units);
    }
    balance.emit(_core);

    const auto metrics = _metrics();
    ASSERT_EQ(metrics.size(), 2U);
    EXPECT_EQ(metrics[0].value, 10.0);
    EXPECT_DOUBLE_EQ(metrics[1].value, 1.6);
}

TEST_F(OpenMpBridgeTest, ThreadsOfTheTeamThatAddNothingCountAsIdle) {
    OpenMpWorkBalance balance("one_worker", 4);
#pragma omp parallel num_threads(4)
    {
        if (omp_get_thread_num() == 0) {
            balance.add(30);
        }
    }
    balance.emit(_core);

    const auto metrics = _metrics();
    ASSERT_EQ(metrics.size(), 2U);
    EXPECT_EQ(metrics[0].value, 30.0);
    EXPECT_EQ(test::tag_of(metrics[0].tags, "threads"), "4");
    EXPECT_DOUBLE_EQ(metrics[1].value, 4.0);  // one thread did everything
}

TEST_F(OpenMpBridgeTest, WithNoWorkOnlyAZeroTotalIsEmitted) {
    const OpenMpWorkBalance balance("nothing", 4);
    balance.emit(_core);

    const auto metrics = _metrics();
    ASSERT_EQ(metrics.size(), 1U);
    EXPECT_EQ(metrics[0].name, "openmp.work.total");
    EXPECT_EQ(metrics[0].value, 0.0);
    EXPECT_EQ(tags_of(metrics[0].tags), (Tags{{"region", "nothing"}, {"threads", "0"}}));
}

TEST_F(OpenMpBridgeTest, ThreadsBeyondTheSlotsAreUnattributedAndSuppressTheImbalance) {
    OpenMpWorkBalance balance("too_many", 2);
#pragma omp parallel num_threads(4)
    { balance.add(1); }
    balance.emit(_core);

    const auto metrics = _metrics();
    ASSERT_EQ(metrics.size(), 2U);
    EXPECT_EQ(metrics[0].name, "openmp.work.total");
    EXPECT_EQ(metrics[0].value, 2.0);
    EXPECT_EQ(metrics[1].name, "openmp.work.unattributed");
    EXPECT_EQ(metrics[1].value, 2.0);
    EXPECT_EQ(tags_of(metrics[1].tags), (Tags{{"region", "too_many"}, {"threads", "4"}}));
}

TEST_F(OpenMpBridgeTest, TheDefaultSlotCountIsTheMaxThreadsAtConstruction) {
    omp_set_num_threads(2);
    OpenMpWorkBalance balance("default_slots");
#pragma omp parallel num_threads(3)
    { balance.add(1); }
    balance.emit(_core);

    const auto metrics = _metrics();
    ASSERT_EQ(metrics.size(), 2U);
    EXPECT_EQ(metrics[0].value, 2.0);
    EXPECT_EQ(metrics[1].name, "openmp.work.unattributed");
    EXPECT_EQ(metrics[1].value, 1.0);
}

TEST_F(OpenMpBridgeTest, AddsFromANestedRegionAreUnattributed) {
    omp_set_max_active_levels(1);  // inner regions get a team of one thread
    OpenMpWorkBalance balance("nested", 2);
#pragma omp parallel num_threads(2)
    {
        balance.add(1);
#pragma omp parallel num_threads(2)
        { balance.add(5); }
    }
    balance.emit(_core);

    const auto metrics = _metrics();
    ASSERT_EQ(metrics.size(), 2U);
    EXPECT_EQ(metrics[0].value, 2.0);
    EXPECT_EQ(metrics[1].name, "openmp.work.unattributed");
    EXPECT_EQ(metrics[1].value, 10.0);
}

TEST_F(OpenMpBridgeTest, ARegionInsideASerialisedOuterRegionIsAttributedToItsOwnTeam) {
    omp_set_max_active_levels(2);
    OpenMpWorkBalance balance("inner", 4);
#pragma omp parallel num_threads(1)
    {
#pragma omp parallel num_threads(4)
        { balance.add(3); }
    }
    balance.emit(_core);

    const auto metrics = _metrics();
    ASSERT_EQ(metrics.size(), 2U);
    EXPECT_EQ(metrics[0].name, "openmp.work.total");
    EXPECT_EQ(metrics[0].value, 12.0);
    EXPECT_EQ(tags_of(metrics[0].tags), (Tags{{"region", "inner"}, {"threads", "4"}}));
    EXPECT_EQ(metrics[1].name, "openmp.work.imbalance");
    EXPECT_DOUBLE_EQ(metrics[1].value, 1.0);
}

TEST_F(OpenMpBridgeTest, WhenNoAddReachesASlotTheThreadsTagIsZero) {
    omp_set_max_active_levels(2);
    OpenMpWorkBalance balance("all_nested", 2);
#pragma omp parallel num_threads(2)
    {
#pragma omp parallel num_threads(2)
        { balance.add(1); }
    }
    balance.emit(_core);

    const auto metrics = _metrics();
    ASSERT_EQ(metrics.size(), 2U);
    EXPECT_EQ(metrics[0].value, 0.0);
    EXPECT_EQ(test::tag_of(metrics[0].tags, "threads"), "0");
    EXPECT_EQ(metrics[1].name, "openmp.work.unattributed");
    EXPECT_EQ(metrics[1].value, 4.0);
}

// The example in the header and in docs/bridges.md.
TEST_F(OpenMpBridgeTest, ADynamicParallelForWithOneUnitPerIterationTotalsItsIterations) {
    omp_set_num_threads(4);
    const int64_t n_events = 1000;
    const auto process = [](int64_t event) { return event * event; };

    lumen::bridges::OpenMpWorkBalance balance("events");
#pragma omp parallel for schedule(dynamic)
    for (int64_t i = 0; i < n_events; ++i) {
        static_cast<void>(process(i));
        balance.add(1);
    }
    balance.emit(_core);

    const auto metrics = _metrics();
    ASSERT_EQ(metrics.size(), 2U);
    EXPECT_EQ(metrics[0].name, "openmp.work.total");
    EXPECT_EQ(metrics[0].value, 1000.0);
    EXPECT_EQ(tags_of(metrics[0].tags), (Tags{{"region", "events"}, {"threads", "4"}}));
    EXPECT_EQ(metrics[1].name, "openmp.work.imbalance");
    EXPECT_GE(metrics[1].value, 1.0);
    EXPECT_LE(metrics[1].value, 4.0);
}

TEST_F(OpenMpBridgeTest, AddsOutsideAParallelRegionCountAsOneThread) {
    OpenMpWorkBalance balance("serial", 4);
    balance.add(2);
    balance.add(3);
    balance.emit(_core);

    const auto metrics = _metrics();
    ASSERT_EQ(metrics.size(), 2U);
    EXPECT_EQ(metrics[0].value, 5.0);
    EXPECT_EQ(test::tag_of(metrics[0].tags, "threads"), "1");
    EXPECT_DOUBLE_EQ(metrics[1].value, 1.0);
}

class OpenMpBridgeGlobalCoreTest : public test::GlobalCoreTest {};

TEST_F(OpenMpBridgeGlobalCoreTest, WithoutACoreArgumentTheRecordsGoToTheGlobalCore) {
    bridges::emit_openmp_configuration();
    core().flush();

    const auto metrics = _captured->metrics();
    ASSERT_EQ(metrics.size(), 2U);
    EXPECT_EQ(metrics[0].name, "openmp.max_threads");
    EXPECT_EQ(metrics[0].value, static_cast<double>(omp_get_max_threads()));
}

}  // namespace
}  // namespace lumen
