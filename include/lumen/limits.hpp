#ifndef LUMEN_LIMITS_HPP
#define LUMEN_LIMITS_HPP

#include <cstddef>

#include "lumen/config.hpp"

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
//
// Memory. A Core allocates its three queues when it is constructed: per
// queue, capacity × (sizeof(record) + 8) bytes, where the 8 is the slot's
// sequence number and the record sizes are in lumen/record.hpp ("Size").
// With the default capacities on a 64-bit platform that is 41 MB for logs,
// 9.3 MB for metrics and 2.4 MB for progress, about 53 MB in all. It is
// address space at first: a page of slots becomes resident when a record is
// first queued on it, and stays resident, so a queue that has wrapped (more
// records have passed through it than it holds) is resident in full. On
// top of that, each queued record holds its string block on the heap until
// it is delivered; delivery frees it unless a sink kept a copy of the
// record, and an empty slot holds no heap memory. Smaller capacities (CMake
// LUMEN_LOG_CAPACITY etc.) reduce the queues in proportion.

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

// Records each queue holds between submission and delivery. Build settings
// (CMake LUMEN_LOG_CAPACITY etc., powers of two), read from lumen/config.hpp.
inline constexpr std::size_t log_queue_capacity = LUMEN_LOG_CAPACITY;
inline constexpr std::size_t metric_queue_capacity = LUMEN_METRIC_CAPACITY;
inline constexpr std::size_t progress_queue_capacity = LUMEN_PROGRESS_CAPACITY;

}  // namespace lumen

#endif
