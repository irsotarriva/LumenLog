#include "lumen/dashboard_model.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace lumen {

namespace {

// Counts below 1 become 1; negative durations become 0.
[[nodiscard]] DashboardModel::Limits at_least_one(DashboardModel::Limits limits) noexcept {
    limits.scrollback_lines = std::max<std::size_t>(limits.scrollback_lines, 1);
    limits.metric_history = std::max<std::size_t>(limits.metric_history, 1);
    limits.max_metrics = std::max<std::size_t>(limits.max_metrics, 1);
    limits.max_progress_bars = std::max<std::size_t>(limits.max_progress_bars, 1);
    limits.finished_bar_linger = std::max(limits.finished_bar_linger, std::chrono::nanoseconds{0});
    limits.idle_bar_timeout = std::max(limits.idle_bar_timeout, std::chrono::nanoseconds{0});
    return limits;
}

void update_range(DashboardModel::MetricSeries& series) noexcept {
    double low = std::numeric_limits<double>::quiet_NaN();
    double high = std::numeric_limits<double>::quiet_NaN();
    for (const double value : series.values) {
        if (!std::isfinite(value)) {
            continue;
        }
        low = std::isnan(low) ? value : std::min(low, value);
        high = std::isnan(high) ? value : std::max(high, value);
    }
    series.min = low;
    series.max = high;
}

// Nanoseconds from `then` to `now`; 0 if `then` is later (clocks of
// different threads are not ordered exactly).
[[nodiscard]] uint64_t age(uint64_t then, uint64_t now) noexcept {
    return now > then ? now - then : 0;
}

}  // namespace

DashboardModel::DashboardModel(Limits limits, std::vector<std::string> metric_names)
    : limits_(at_least_one(limits)), fixed_metrics_(!metric_names.empty()) {
    for (std::string& name : metric_names) {
        metrics_.push_back({.name = std::move(name), .values = {}, .min = 0.0, .max = 0.0});
    }
    for (MetricSeries& series : metrics_) {
        update_range(series);
    }
}

void DashboardModel::add_log(LogLevel level, std::string text) {
    if (scrollback_.size() >= limits_.scrollback_lines) {
        scrollback_.pop_front();
    }
    scrollback_.push_back({.level = level, .text = std::move(text)});
}

void DashboardModel::add_metric(std::string_view name, double value) {
    MetricSeries* series = find_or_add_series_(name);
    if (series == nullptr) {
        return;  // not a tracked metric
    }
    if (series->values.size() >= limits_.metric_history) {
        series->values.pop_front();
    }
    series->values.push_back(value);
    update_range(*series);
}

DashboardModel::MetricSeries* DashboardModel::find_or_add_series_(std::string_view name) {
    const auto found = std::ranges::find(metrics_, name, &MetricSeries::name);
    if (found != metrics_.end()) {
        return &*found;
    }
    if (fixed_metrics_ || metrics_.size() >= limits_.max_metrics) {
        return nullptr;
    }
    metrics_.push_back({.name = std::string(name), .values = {}, .min = 0.0, .max = 0.0});
    return &metrics_.back();
}

void DashboardModel::add_progress(const ProgressRecord& record) {
    const auto found = std::ranges::find(bars_, record.id, &ProgressBar::id);
    if (found != bars_.end()) {
        found->current = record.current;
        found->total = record.total;
        found->last_update_ns = record.timestamp_ns;
        return;
    }
    if (bars_.size() >= limits_.max_progress_bars) {
        evict_one_bar_();
    }
    bars_.push_back({.id = record.id,
                     .label = std::string(record.label),
                     .current = record.current,
                     .total = record.total,
                     .last_update_ns = record.timestamp_ns});
}

void DashboardModel::evict_one_bar_() {
    const auto older = [](const ProgressBar& a, const ProgressBar& b) {
        return a.last_update_ns < b.last_update_ns;
    };
    auto victim = bars_.end();
    for (auto it = bars_.begin(); it != bars_.end(); ++it) {
        if (it->finished() && (victim == bars_.end() || older(*it, *victim))) {
            victim = it;
        }
    }
    if (victim == bars_.end()) {
        victim = std::ranges::min_element(bars_, older);
    }
    if (victim != bars_.end()) {
        bars_.erase(victim);
    }
}

void DashboardModel::expire_progress(uint64_t now_ns) {
    const auto linger = static_cast<uint64_t>(limits_.finished_bar_linger.count());
    const auto idle = static_cast<uint64_t>(limits_.idle_bar_timeout.count());
    std::erase_if(bars_, [&](const ProgressBar& bar) {
        const uint64_t since = age(bar.last_update_ns, now_ns);
        return (bar.finished() && since >= linger) || (idle > 0 && since >= idle);
    });
}

}  // namespace lumen
