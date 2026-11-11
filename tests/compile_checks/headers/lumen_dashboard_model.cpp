// lumen/dashboard_model.hpp alone declares what it documents.
#include "lumen/dashboard_model.hpp"

#include <type_traits>
#include <utility>

namespace lumen {

static_assert(
    std::is_constructible_v<DashboardModel, DashboardModel::Limits, std::vector<std::string>>);
using Model = DashboardModel;
static_assert(std::is_same_v<decltype(&Model::add_log), void (Model::*)(LogLevel, std::string)>);
static_assert(
    std::is_same_v<decltype(&Model::add_metric), void (Model::*)(std::string_view, double)>);
static_assert(
    std::is_same_v<decltype(&Model::add_progress), void (Model::*)(const ProgressRecord&)>);
static_assert(std::is_same_v<decltype(&Model::expire_progress), void (Model::*)(uint64_t)>);
static_assert(std::is_same_v<decltype(std::declval<const Model&>().scrollback()),
                             const std::deque<Model::LogLine>&>);
static_assert(std::is_same_v<decltype(std::declval<const Model&>().metrics()),
                             const std::vector<Model::MetricSeries>&>);
static_assert(std::is_same_v<decltype(std::declval<const Model&>().progress_bars()),
                             const std::vector<Model::ProgressBar>&>);

}  // namespace lumen
