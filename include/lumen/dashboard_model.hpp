#ifndef LUMEN_DASHBOARD_MODEL_HPP
#define LUMEN_DASHBOARD_MODEL_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

#include "lumen/level.hpp"
#include "lumen/record.hpp"

// What DashboardSink (lumen/dashboard_sink.hpp) shows, kept apart from how it
// is drawn: recent log lines, a short history of chosen metrics, and the
// progress bars in flight. Built in every configuration (it needs no FTXUI),
// so a program can also drive it to draw its own view.
//
// Not synchronised: one thread at a time. Copyable, so that a renderer can
// take a snapshot and draw it without holding the owner's lock.

namespace lumen {

class DashboardModel {
public:
    struct Limits {
        std::size_t scrollback_lines = 512;  // log lines kept, oldest dropped first
        std::size_t metric_history = 128;    // values kept per metric, oldest dropped first
        std::size_t max_metrics = 8;         // metrics tracked when no names are given
        std::size_t max_progress_bars = 16;  // bars shown at once
        // A finished bar stays this long after its last record, so that its
        // completion can be seen.
        std::chrono::nanoseconds finished_bar_linger = std::chrono::seconds{2};
        // Any bar whose last record is older than this is removed: the rule
        // for bars that never finish, since a ProgressHandle destroyed
        // without finish() sends nothing further. Zero: never.
        std::chrono::nanoseconds idle_bar_timeout = std::chrono::seconds{60};
    };

    struct LogLine {
        LogLevel level = LogLevel::Info;
        std::string text;
    };

    struct MetricSeries {
        std::string name;
        std::deque<double> values;  // oldest first
        // Over the finite values held; both NaN when none is finite.
        double min = 0.0;
        double max = 0.0;
    };

    struct ProgressBar {
        uint64_t id = 0;
        std::string label;
        uint64_t current = 0;
        uint64_t total = 0;
        uint64_t last_update_ns = 0;  // timestamp of the bar's latest record

        // A bar with total == 0 has no end: it shows a count and is never
        // finished, only removed when idle.
        [[nodiscard]] bool finished() const noexcept { return total > 0 && current >= total; }
    };

    // A count limit of 0 is taken as 1, a negative duration as 0.
    // `metric_names`: the metrics to track, in display order; empty: the first max_metrics names
    // seen, in that order.
    DashboardModel(Limits limits, std::vector<std::string> metric_names);

    void add_log(LogLevel level, std::string text);
    void add_metric(std::string_view name, double value);

    // Updates the bar with the record's id in place, or appends a new bar
    // after the existing ones: bars keep the order in which they first
    // appeared. When max_progress_bars are shown, a new bar first evicts the
    // finished bar with the oldest update, or if none is finished, the bar
    // with the oldest update.
    void add_progress(const ProgressRecord& record);

    // Removes, as of `now_ns` (lumen::now_ns() time), finished bars older
    // than finished_bar_linger and any bar older than idle_bar_timeout.
    void expire_progress(uint64_t now_ns);

    [[nodiscard]] const std::deque<LogLine>& scrollback() const noexcept { return scrollback_; }
    [[nodiscard]] const std::vector<MetricSeries>& metrics() const noexcept { return metrics_; }
    [[nodiscard]] const std::vector<ProgressBar>& progress_bars() const noexcept { return bars_; }

private:
    [[nodiscard]] MetricSeries* find_or_add_series_(std::string_view name);
    void evict_one_bar_();

    Limits limits_;
    bool fixed_metrics_;  // true when the caller named the metrics
    std::deque<LogLine> scrollback_;
    std::vector<MetricSeries> metrics_;
    std::vector<ProgressBar> bars_;
};

}  // namespace lumen

#endif
