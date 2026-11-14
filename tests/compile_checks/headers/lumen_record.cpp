// lumen/record.hpp alone declares what it documents.
#include "lumen/record.hpp"

#include <type_traits>

namespace lumen {

static_assert(std::is_same_v<RecordTags, TagSet<max_record_tags>>);
static_assert(std::is_same_v<decltype(LogRecord{}.level), LogLevel>);
static_assert(std::is_same_v<decltype(LogRecord{}.message), std::string_view>);
static_assert(std::is_same_v<decltype(LogRecord{}.tags), RecordTags>);
static_assert(std::is_same_v<decltype(LogRecord{}.timestamp_ns), uint64_t>);
static_assert(std::is_same_v<decltype(LogRecord{}.thread_id), uint32_t>);
static_assert(std::is_same_v<decltype(LogRecord{}.source), SourceLocation>);
static_assert(std::is_same_v<decltype(LogRecord{}.storage), RecordStorage>);
static_assert(std::is_same_v<decltype(SourceLocation{}.class_name), std::string_view>);
static_assert(std::is_same_v<decltype(SourceLocation{}.line), uint32_t>);
static_assert(std::is_same_v<decltype(MetricRecord{}.name), std::string_view>);
static_assert(std::is_same_v<decltype(MetricRecord{}.value), double>);
static_assert(std::is_same_v<decltype(MetricRecord{}.storage), RecordStorage>);
static_assert(std::is_same_v<decltype(ProgressRecord{}.id), uint64_t>);
static_assert(std::is_same_v<decltype(ProgressRecord{}.label), std::string_view>);
static_assert(std::is_same_v<decltype(ProgressRecord{}.current), uint64_t>);
static_assert(std::is_same_v<decltype(ProgressRecord{}.total), uint64_t>);
static_assert(std::is_same_v<decltype(ProgressRecord{}.storage), RecordStorage>);

}  // namespace lumen
