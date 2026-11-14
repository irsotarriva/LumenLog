// lumen/dashboard_sink.hpp alone declares what it documents. Only
// unevaluated uses: the definitions exist only with LUMEN_ENABLE_DASHBOARD=ON.
#include "lumen/dashboard_sink.hpp"

#include <type_traits>

namespace lumen {

static_assert(std::is_base_of_v<Sink, DashboardSink>);
static_assert(std::is_same_v<decltype(DashboardSink::create(DashboardSink::Config{})),
                             std::expected<std::unique_ptr<DashboardSink>, std::error_code>>);
static_assert(std::is_same_v<decltype(DashboardSink::Config{}.limits), DashboardModel::Limits>);
static_assert(
    std::is_same_v<decltype(DashboardSink::Config{}.refresh_interval), std::chrono::milliseconds>);
static_assert(std::is_same_v<decltype(DashboardSink::Config{}.diagnostics), DiagnosticHandler>);

}  // namespace lumen
