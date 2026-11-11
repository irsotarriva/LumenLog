#ifndef LUMEN_RECORD_HPP
#define LUMEN_RECORD_HPP

#include <cstdint>
#include <memory>
#include <string_view>

#include "lumen/level.hpp"
#include "lumen/limits.hpp"
#include "lumen/tag_set.hpp"

// The three record types that sinks receive.
//
// Ownership. A record that reaches a sink owns its strings: the message (or
// name, or label) and every tag key and value point into `storage`, one
// immutable NUL-terminated block allocated when the record was submitted and
// shared by every copy of the record. A sink may copy a record and keep it;
// the views stay valid as long as any copy is alive. The strings in `source`
// usually come from std::source_location, live for the whole program and are
// not copied; a source given as text (a call site in another language, see
// BasicRecordBuilder) is copied into `storage` too.
//
// Completeness. Records are only created by Core's producers (the builders
// in lumen/record_builder.hpp and ProgressHandle in lumen/progress.hpp). Each
// one arrives with its timestamp, the producing thread's id, its explicit
// tags and the ambient context merged after them (lumen/context.hpp), and no
// key twice.

namespace lumen {

using RecordTags = TagSet<max_record_tags>;

struct SourceLocation {
    std::string_view file;
    std::string_view function;
    std::string_view class_name;  // innermost enclosing class, "" for a free function
    uint32_t line{};
};

namespace detail {
// The array form of shared_ptr: one block for all of a record's strings.
using RecordStorage = std::shared_ptr<const char[]>;  // NOLINT(*-avoid-c-arrays)
}  // namespace detail

struct LogRecord {
    LogLevel level{};
    std::string_view message;
    RecordTags tags{};
    uint64_t timestamp_ns{};  // lumen::now_ns() at submission
    uint32_t thread_id{};     // lumen::this_thread_id() of the producer
    SourceLocation source{};
    detail::RecordStorage storage;
};

struct MetricRecord {
    std::string_view name;
    double value{};
    RecordTags tags{};
    uint64_t timestamp_ns{};
    uint32_t thread_id{};
    detail::RecordStorage storage;
};

// One state of a progress bar. `current` counts completed units of work out
// of `total`; every record for one bar carries the same `id`. The semantics of
// the sequence are described with ProgressHandle (lumen/progress.hpp).
struct ProgressRecord {
    uint64_t id{};
    std::string_view label;
    uint64_t current{};
    uint64_t total{};
    RecordTags tags{};
    uint64_t timestamp_ns{};
    uint32_t thread_id{};
    detail::RecordStorage storage;
};

}  // namespace lumen

#endif
