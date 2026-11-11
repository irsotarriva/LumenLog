// lumen/record_builder.hpp alone declares what it documents.
#include "lumen/record_builder.hpp"

#include <type_traits>

namespace lumen {

static_assert(std::is_same_v<RecordBuilder, BasicRecordBuilder<LogRecord>>);
static_assert(std::is_same_v<MetricBuilder, BasicRecordBuilder<MetricRecord>>);
static_assert(std::is_same_v<decltype(metric("loss", 0.25)), MetricBuilder>);
static_assert(std::is_constructible_v<RecordBuilder, Core&, LogLevel, std::source_location,
                                      std::string_view>);
static_assert(
    std::is_constructible_v<RecordBuilder, Core&, LogLevel, std::source_location, const char*>);
static_assert(std::is_constructible_v<RecordBuilder, Core&, LogLevel, const SourceLocation&,
                                      std::string_view>);
static_assert(std::is_constructible_v<MetricBuilder, Core&, std::string_view, double>);
static_assert(std::is_same_v<decltype(metric("m", 1.0).tag("k", "v")), MetricBuilder&>);
static_assert(std::is_same_v<decltype(metric("m", 1.0).tag("k", 1)), MetricBuilder&>);
static_assert(std::is_same_v<decltype(metric("m", 1.0).tag("k", 1.5)), MetricBuilder&>);
static_assert(std::is_same_v<decltype(metric("m", 1.0).tag("k", true)), MetricBuilder&>);

}  // namespace lumen
