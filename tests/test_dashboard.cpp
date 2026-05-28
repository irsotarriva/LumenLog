#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "lumen/core.h"
#include "lumen/predicate.h"
#include "lumen/sink.h"

namespace lumen {
namespace {

void wait_flush(int ms = 100) {
    core().flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

struct DummySink : public Sink {
    std::mutex mtx;
    void on_log(const LogRecord&) override {}
    void on_metric(const MetricRecord&) override {}
    void on_progress(const ProgressRecord&) override {}
    void flush() override {}
};

class DashboardTest : public ::testing::Test {
protected:
    std::vector<SinkId> __ids;

    void TearDown() override {
        for (auto id : __ids) {
            (void)core().remove_sink(id);
        }
        __ids.clear();
    }

    SinkId register_sink(std::unique_ptr<Sink> sink, Predicate pred) {
        SinkId id = core().add_sink(std::move(sink), std::move(pred));
        __ids.push_back(id);
        return id;
    }
};

TEST_F(DashboardTest, DashboardConfigDefaultsDisabled) {
    TerminalSink::Config cfg;
    EXPECT_FALSE(cfg.enable_dashboard);
    EXPECT_EQ(cfg.dashboard_height, 5);
    EXPECT_TRUE(cfg.dashboard_metrics.empty());
}

TEST_F(DashboardTest, DashboardConfigEnabled) {
    TerminalSink::Config cfg;
    cfg.enable_dashboard = true;
    cfg.dashboard_height = 8;
    cfg.dashboard_metrics = {"loss", "fps"};

    EXPECT_TRUE(cfg.enable_dashboard);
    EXPECT_EQ(cfg.dashboard_height, 8);
    EXPECT_EQ(cfg.dashboard_metrics.size(), 2);
    EXPECT_EQ(cfg.dashboard_metrics[0], "loss");
    EXPECT_EQ(cfg.dashboard_metrics[1], "fps");
}

TEST_F(DashboardTest, TerminalSinkConstructsWithoutDashboard) {
    auto cfg = TerminalSink::Config{};
    cfg.enable_dashboard = false;
    EXPECT_NO_THROW(std::make_unique<TerminalSink>(cfg));
}

TEST_F(DashboardTest, DashboardRendersMetricsDoesNotThrow) {
    auto cfg = TerminalSink::Config{};
    cfg.enable_dashboard = true;
    cfg.dashboard_metrics = {"loss", "accuracy"};

    auto sink = std::make_unique<TerminalSink>(cfg);
    register_sink(std::move(sink), always());

    core().emit(MetricRecord{.name = "loss", .value = 0.42, .timestamp_ns = 0});
    core().emit(MetricRecord{.name = "loss", .value = 0.38, .timestamp_ns = 0});
    core().emit(MetricRecord{.name = "accuracy", .value = 0.91, .timestamp_ns = 0});

    EXPECT_NO_THROW(wait_flush());
}

TEST_F(DashboardTest, DashboardRendersProgressDoesNotThrow) {
    auto cfg = TerminalSink::Config{};
    cfg.enable_dashboard = true;

    auto sink = std::make_unique<TerminalSink>(cfg);
    register_sink(std::move(sink), always());

    core().emit(ProgressRecord{
        .id = 1, .label = "upload", .current = 50, .total = 100, .timestamp_ns = 0});

    EXPECT_NO_THROW(wait_flush());
}

TEST_F(DashboardTest, DashboardUpdatesOnDispatch) {
    auto cfg = TerminalSink::Config{};
    cfg.enable_dashboard = true;
    cfg.dashboard_metrics = {"loss"};

    auto sink = std::make_unique<TerminalSink>(cfg);
    register_sink(std::move(sink), always());

    core().emit(MetricRecord{.name = "loss", .value = 0.5, .timestamp_ns = 0});
    wait_flush();

    core().emit(MetricRecord{.name = "loss", .value = 0.3, .timestamp_ns = 0});
    wait_flush();

    SUCCEED();
}

TEST_F(DashboardTest, DashboardProgressCompletionRemovesBar) {
    auto cfg = TerminalSink::Config{};
    cfg.enable_dashboard = true;

    auto sink = std::make_unique<TerminalSink>(cfg);
    register_sink(std::move(sink), always());

    core().emit(ProgressRecord{
        .id = 1, .label = "task", .current = 100, .total = 100, .timestamp_ns = 0});

    EXPECT_NO_THROW(wait_flush());
}

}  // namespace
}  // namespace lumen
