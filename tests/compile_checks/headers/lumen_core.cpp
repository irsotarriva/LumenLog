// lumen/core.hpp alone declares what it documents.
#include "lumen/core.hpp"

#include <type_traits>

namespace lumen {

static_assert(std::is_same_v<decltype(core()), Core&>);
static_assert(std::is_same_v<decltype(stderr_diagnostics()), DiagnosticHandler>);
static_assert(std::is_same_v<decltype(core().add_sink(std::unique_ptr<Sink>{})),
                             std::expected<SinkId, std::error_code>>);
static_assert(std::is_same_v<decltype(core().add_sink(std::unique_ptr<Sink>{}, always())),
                             std::expected<SinkId, std::error_code>>);
static_assert(std::is_same_v<decltype(core().remove_sink(SinkId{})),
                             std::expected<std::unique_ptr<Sink>, std::error_code>>);
static_assert(std::is_same_v<decltype(core().sink_exception_count(SinkId{})),
                             std::expected<uint64_t, std::error_code>>);
static_assert(std::is_same_v<decltype(core().flush()), void>);
static_assert(std::is_same_v<decltype(core().statistics()), CoreStatistics>);
static_assert(std::is_same_v<decltype(core().set_diagnostic_handler(DiagnosticHandler{})), void>);
static_assert(std::is_constructible_v<Core, DiagnosticHandler>);
static_assert(std::is_same_v<decltype(CoreStatistics{}.logs), QueueStatistics>);
static_assert(std::is_same_v<decltype(CoreStatistics{}.tags_dropped), uint64_t>);
static_assert(std::is_same_v<decltype(QueueStatistics{}.accepted), uint64_t>);
static_assert(std::is_same_v<decltype(QueueStatistics{}.dropped_queue_full), uint64_t>);
static_assert(std::is_same_v<decltype(QueueStatistics{}.dropped_after_shutdown), uint64_t>);

}  // namespace lumen
