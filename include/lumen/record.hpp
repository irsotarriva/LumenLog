#ifndef LUMEN_RECORD_HPP
#define LUMEN_RECORD_HPP

#include <concepts>
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
// immutable block allocated when the record was submitted and shared by
// every copy of the record. Each of these strings is followed by a NUL in the
// block, so `view.data()` may be passed where a C string is expected. A sink
// may copy a record and keep it; the views stay valid as long as any copy is
// alive. The strings in `source` usually come from std::source_location, live
// for the whole program and are not copied; a source given as text (a call
// site in another language, see BasicRecordBuilder) is copied into `storage`
// too.
//
// Completeness. Records are only created by Core's producers (the builders
// in lumen/record_builder.hpp and ProgressHandle in lumen/progress.hpp). Each
// one arrives with its timestamp, the producing thread's id, its explicit
// tags and the ambient context merged after them (lumen/context.hpp), and no
// key twice.
//
// Size. The tags are held inline, max_record_tags slots of two views each
// (RecordTags is 520 bytes on 64-bit platforms), so a record is large: on a
// 64-bit platform LogRecord is 632 bytes, MetricRecord 576 and
// ProgressRecord 592, and each is at most 640 (checked below). Every queue
// slot holds one record, which is what a queue's memory is made of
// (lumen/limits.hpp, "Memory"); the strings are not part of it.

namespace lumen {

using RecordTags = TagSet<max_record_tags>;

struct SourceLocation {
    std::string_view file;
    std::string_view function;
    std::string_view class_name;  // innermost enclosing class, "" for a free function
    uint32_t line{};
};

// The array form of shared_ptr: one block for all of a record's strings.
using RecordStorage = std::shared_ptr<const char[]>;  // NOLINT(*-avoid-c-arrays)

struct LogRecord {
    LogLevel level{};
    std::string_view message;
    RecordTags tags{};
    uint64_t timestamp_ns{};  // lumen::now_ns() at submission
    uint32_t thread_id{};     // lumen::this_thread_id() of the producer
    SourceLocation source{};
    RecordStorage storage;
};

struct MetricRecord {
    std::string_view name;
    double value{};
    RecordTags tags{};
    uint64_t timestamp_ns{};
    uint32_t thread_id{};
    RecordStorage storage;
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
    RecordStorage storage;
};

// One of the three record types above. The one definition of "a record";
// narrower sets (such as the records a builder makes) are written in terms
// of it.
template <typename R>
concept RecordType =
    std::same_as<R, LogRecord> || std::same_as<R, MetricRecord> || std::same_as<R, ProgressRecord>;

// The bound "Size" above states (640 bytes), on the platforms it is stated for.
static_assert(sizeof(void*) != 8 || (sizeof(LogRecord) <= 640 && sizeof(MetricRecord) <= 640 &&
                                     sizeof(ProgressRecord) <= 640),
              "update \"Size\" at the top of lumen/record.hpp");

}  // namespace lumen

#endif
