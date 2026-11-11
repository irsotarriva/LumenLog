// lumen/lumen.hpp alone brings in the headers a program that logs needs
// (the first table of docs/api.md).
#include "lumen/lumen.hpp"

#include <type_traits>

namespace lumen {

static_assert(std::is_same_v<decltype(core()), Core&>);
static_assert(std::is_same_v<decltype(metric("m", 1.0)), MetricBuilder>);
static_assert(std::is_same_v<decltype(progress("p", 1)), ProgressHandle>);
static_assert(
    std::is_same_v<decltype(set_process_tag("k", "v")), std::expected<void, std::error_code>>);
static_assert(std::is_class_v<Tagged>);
static_assert(std::is_class_v<RecordTags>);
static_assert(std::is_same_v<decltype(parse_predicate("true")),
                             std::expected<Predicate, PredicateParseError>>);
static_assert(std::is_same_v<decltype(make_error_code(LumenError::NullSink)), std::error_code>);
static_assert(std::is_abstract_v<Sink>);
static_assert(std::is_base_of_v<Sink, TerminalSink>);
static_assert(std::is_base_of_v<Sink, FileSink>);
static_assert(std::is_same_v<decltype(now_ns()), uint64_t>);
static_assert(max_record_tags > 0);
static_assert(level_compiled_in(LogLevel::Fatal));

}  // namespace lumen
