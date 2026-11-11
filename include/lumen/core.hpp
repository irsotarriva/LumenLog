#ifndef LUMEN_CORE_HPP
#define LUMEN_CORE_HPP

#include <concepts>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string_view>
#include <system_error>

#include "lumen/predicate.hpp"
#include "lumen/record.hpp"
#include "lumen/sink.hpp"

// Core: the broker between producers and sinks.
//
// Flow. A producer (a logging macro, lumen::metric, a ProgressHandle) builds
// a record on its own thread and submits it to a Core. Submission stamps the
// record (timestamp, thread id), merges the ambient context
// (lumen/context.hpp), copies every string into the record's own storage (one
// allocation) and pushes the record into a bounded lock-free queue, one per
// record type. A dispatch thread owned by the Core pops records and calls the
// sinks whose predicate matches.
//
// Threads.
//   - Producers: any number of threads. Submission takes no lock (except a
//     refresh of the thread's copy of the process tags after they changed)
//     and makes no system call unless the dispatch thread is asleep.
//   - Sink callbacks (on_log, on_metric, on_progress, flush): one thread at a
//     time. Usually the dispatch thread; flush() delivers on its caller's
//     thread instead. A sink is never called concurrently with itself or with
//     another sink of the same Core.
//   - add_sink, remove_sink, sink_exception_count, statistics: any thread,
//     including from inside a sink callback. No lock is held while a sink
//     callback runs, so these never wait for one, with the single exception
//     stated at remove_sink.
//
// Order. Records of one type are delivered in the order they were queued.
// Each pass of the dispatcher delivers the progress records it finds, then the
// metrics, then the logs, so records of different types are not in time
// order with each other.
//
// Loss. A record is refused when its queue is full (capacities in
// lumen/limits.hpp). It is counted in statistics(), and the first refusal
// is reported once through the diagnostic handler. Nothing else is lost
// while the Core is running.
//
// Lifetime of the global core(). It is created on first use and never
// destroyed. When static destruction reaches the point where a function-local
// static constructed at that first use would be destroyed, the core shuts
// down: it stops its dispatch thread, delivers what is queued, flushes and
// destroys its sinks. A record submitted after that (for instance from the
// destructor of a static object that was constructed before the first use of
// core()) is not delivered: it is counted in
// statistics().<kind>.dropped_after_shutdown, and that is all. This is well
// defined; nothing touches a destroyed object.
//
// A Core you construct yourself shuts down the same way in its destructor.
// Producers must not use it after that (they hold a reference to it).
//
// One race is not covered: a record that another thread submits while
// shutdown is in progress either is delivered, or is counted as
// dropped_after_shutdown, or, if its push lands after the final delivery
// pass, stays in the queue undelivered and uncounted. Stop producer threads
// before shutdown to avoid it.

namespace lumen {

using SinkId = uint64_t;

// Receives one line of text about a problem Core cannot return to anyone: the
// first record refused by a full queue, the first tag left out of a full
// record, the first exception thrown by each sink. Called on the thread that
// delivers records, never concurrently with itself for one Core. An empty
// handler discards the lines.
using DiagnosticHandler = std::function<void(std::string_view line)>;

// The handler a Core starts with: writes each line to std::cerr.
[[nodiscard]] DiagnosticHandler stderr_diagnostics();

struct QueueStatistics {
    uint64_t accepted = 0;                // queued for delivery
    uint64_t dropped_queue_full = 0;      // refused: the queue was full
    uint64_t dropped_after_shutdown = 0;  // submitted after the Core shut down
};

// Counters since the Core was created. Each value is exact when read while
// no producer is submitting; otherwise each is a snapshot taken at a slightly
// different moment.
struct CoreStatistics {
    QueueStatistics logs;
    QueueStatistics metrics;
    QueueStatistics progress;
    // Tags left out because the record already held max_record_tags tags:
    // explicit .tag() calls past the limit plus ambient tags that did not fit.
    uint64_t tags_dropped = 0;
};

template <typename R>
concept BuiltRecord = std::same_as<R, LogRecord> || std::same_as<R, MetricRecord>;

template <BuiltRecord R>
class BasicRecordBuilder;
class ProgressHandle;

class Core {
public:
    explicit Core(DiagnosticHandler diagnostics = stderr_diagnostics());
    ~Core();
    Core(const Core&) = delete;
    Core& operator=(const Core&) = delete;
    Core(Core&&) = delete;
    Core& operator=(Core&&) = delete;

    // Registers `sink`; it receives every record dispatched after this call
    // returns for which `predicate` is true (log records are tested with
    // their level, metrics and progress without). Fails with
    // LumenError::NullSink if `sink` is null.
    [[nodiscard]] std::expected<SinkId, std::error_code> add_sink(std::unique_ptr<Sink> sink,
                                                                  Predicate predicate = always());

    // Unregisters a sink and gives it back to the caller.
    //
    // When this returns successfully, the sink is not inside any callback and
    // Core will never call it again; records still queued are not delivered
    // to it. If one of its callbacks is running on another thread, this waits
    // for that callback to return (the one case where a call here waits for a
    // sink). Called from inside a callback of the same Core, it never waits:
    // only the calling sink can be busy then, and removing the calling sink
    // itself fails with LumenError::SinkBusy and leaves it registered.
    // Fails with LumenError::InvalidSinkId if no sink has this id.
    [[nodiscard]] std::expected<std::unique_ptr<Sink>, std::error_code> remove_sink(SinkId id);

    // Exceptions that escaped this sink's callbacks. Sinks should not throw,
    // but one that does is contained: the exception is counted, the first one
    // per sink is reported through the diagnostic handler, and delivery
    // continues with the next sink.
    [[nodiscard]] std::expected<uint64_t, std::error_code> sink_exception_count(SinkId id) const;

    // Delivers every record submitted before the call, then calls flush() on
    // every sink, and returns when all of that is done. Delivery happens on
    // the calling thread (serialised with the dispatch thread).
    //
    // From inside a sink callback of this Core it does nothing and returns at
    // once: the calling thread is the one delivering, and cannot wait for
    // itself. The records stay queued and are delivered when the current pass
    // continues. After shutdown it does nothing.
    void flush();

    [[nodiscard]] CoreStatistics statistics() const noexcept;

    // Replaces the diagnostic handler; an empty one silences diagnostics.
    void set_diagnostic_handler(DiagnosticHandler handler);

private:
    template <BuiltRecord R>
    friend class BasicRecordBuilder;
    friend class ProgressHandle;
    friend Core& core();

    // Where a log record's source strings live. Static: they come from
    // std::source_location and live for the whole program, so they are not
    // copied. Copied: they were given as text (a call site in another
    // language) and are copied into the record's storage with its other
    // strings.
    enum class SourceStrings : uint8_t { Static, Copied };

    // The one way records enter: stamps, merges context, owns the strings,
    // queues. `tags_dropped` counts explicit tags the builder could not hold.
    void submit_(LogRecord&& record, uint32_t tags_dropped, SourceStrings source) noexcept;
    void submit_(MetricRecord&& record, uint32_t tags_dropped) noexcept;
    void submit_(ProgressRecord&& record, uint32_t tags_dropped) noexcept;
    // For a Fatal record: delivers everything queued, then this record, flushes
    // the sinks and terminates (see BasicRecordBuilder).
    [[noreturn]] void submit_fatal_(LogRecord& record, uint32_t tags_dropped,
                                    SourceStrings source) noexcept;

    // Shuts the global core down at static destruction (see above).
    struct ShutdownAtExit;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The process-wide Core used by the logging macros, lumen::metric and
// lumen::progress. See "Lifetime of the global core()" above.
[[nodiscard]] Core& core();

}  // namespace lumen

#endif
