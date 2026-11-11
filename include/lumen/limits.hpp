#ifndef LUMEN_LIMITS_HPP
#define LUMEN_LIMITS_HPP

#include <cstddef>

// Every fixed capacity in Lumen: how many tags and queued records there can
// be. What does not fit is refused and reported:
//
//   limit               refused by                  reported as
//   max_record_tags     .tag() on a builder;        Core::statistics().tags_dropped
//                       the context merge
//   max_process_tags    set_process_tag()           LumenError::TagTableFull
//   max_thread_tags     set_thread_tag()            LumenError::TagTableFull
//   max_instance_tags   Tagged::lumen_tag()         LumenError::TagTableFull
//   *_queue_capacity    submission to a full queue  Core::statistics().<kind>.dropped_queue_full
//
// Scopes (LUMEN_SCOPE, LUMEN_LOOP) have no limit of their own: each one adds
// one tag, at any nesting depth, and only max_record_tags bounds how many of
// them reach a record.
//
// Not bounded here: the length of a message, tag key or tag value (each is
// copied whole, so one long string costs memory in proportion to its
// length), the nesting depth of scopes, and the number of sinks registered
// with a Core. The shipped sinks bound their own line queues
// (lumen/writer_statistics.hpp).

// The queue capacities are build settings (CMake LUMEN_LOG_CAPACITY etc.,
// powers of two). The fallbacks here must equal the CMake defaults
// (cmake/build_settings.cmake); tests/compile_checks/header_defaults.cpp
// checks that they do.
#ifndef LUMEN_LOG_CAPACITY
#define LUMEN_LOG_CAPACITY 65536
#endif
#ifndef LUMEN_METRIC_CAPACITY
#define LUMEN_METRIC_CAPACITY 16384
#endif
#ifndef LUMEN_PROGRESS_CAPACITY
#define LUMEN_PROGRESS_CAPACITY 4096
#endif

namespace lumen {

// Tags on one record: explicit tags first, then ambient context in priority
// order, until the record is full.
inline constexpr std::size_t max_record_tags = 16;

// Distinct keys in the process-wide context (set_process_tag).
inline constexpr std::size_t max_process_tags = 16;

// Distinct keys in one thread's context (set_thread_tag).
inline constexpr std::size_t max_thread_tags = 8;

// Distinct keys on one Tagged object (Tagged::lumen_tag).
inline constexpr std::size_t max_instance_tags = 8;

// Records each queue holds between submission and delivery.
inline constexpr std::size_t log_queue_capacity = LUMEN_LOG_CAPACITY;
inline constexpr std::size_t metric_queue_capacity = LUMEN_METRIC_CAPACITY;
inline constexpr std::size_t progress_queue_capacity = LUMEN_PROGRESS_CAPACITY;

}  // namespace lumen

#endif
