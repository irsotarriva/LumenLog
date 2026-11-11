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
// allocation by the Core; refreshing the thread's copy of the process tags
// and a message longer than the builder's buffer allocate too) and pushes the
// record into a bounded lock-free queue, one per record type. A dispatch
// thread owned by the Core pops records and calls the sinks whose predicate
// matches.
//
// Threads.
//   - Producers: any number of threads. Submission takes no lock (except a
//     refresh of the thread's copy of the process tags after they changed)
//     and makes no system call of its own unless the dispatch thread is
//     asleep. The memory allocator may make one (to grow a thread's heap).
//   - Sink callbacks (on_log, on_metric, on_progress, flush): one thread at a
//     time. Usually the dispatch thread; flush() delivers on its caller's
//     thread instead. A sink is never called concurrently with itself or with
//     another sink of the same Core.
//   - If the dispatch thread cannot be started (the system refuses a new
//     thread), the constructor does not throw. The Core reports it through
//     the diagnostic handler and then delivers each record on the thread
//     that submits it, right after queuing it, under the same lock that
//     serialises delivery (so submission takes that lock). A record a sink
//     submits from inside its callback waits for the next submission or
//     flush(). Everything else is unchanged.
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
// Fatal records (LogLevel::Fatal, see BasicRecordBuilder) are not queued: the
// thread that raises one delivers, on its own stack, everything queued
// before it, then the fatal record, flushes every sink and calls
// std::terminate(). Raised from inside a sink callback, the record whose
// callback is running is first handed to the sinks that had not received it
// yet; the sink whose callback raised the fatal record receives nothing more
// (it is in the middle of a call). When several threads raise fatal records
// at once, each one whose submission began before the delivering thread
// finished is delivered, in the order they were submitted, before the
// process terminates; one raised after that moment is not.
//
// Lifetime of the global core(). It is created on first use and never
// destroyed. When static destruction reaches the point where a function-local
// static constructed at that first use would be destroyed, the core shuts
// down: it stops its dispatch thread, delivers what is queued, flushes and
// destroys its sinks. A record submitted after that (for instance from the
// destructor of a static object that was constructed before the first use of
// core()) is not delivered: it is counted in
// statistics().<kind>.dropped_after_shutdown, and that is all; it is neither
// stamped nor merged with the ambient context. The Core object itself is
// never destroyed, so a record submitted at any point of static destruction
// reaches a valid Core. A record submitted before the shutdown point (from a
// static destroyed earlier, or from a thread_local destructor) is submitted
// as usual, which reads the calling thread's ambient context: whether that
// context is still readable then is stated in lumen/context.hpp, not here.
//
// A Core you construct yourself shuts down the same way in its destructor.
// Producers must not use it after that (they hold a reference to it).
//
// The sinks are destroyed last, with no lock held, so a sink's destructor may
// call its Core: add_sink then fails with LumenError::CoreShutDown,
// remove_sink and sink_exception_count with LumenError::InvalidSinkId (no
// sink is registered any more), flush() does nothing and records are counted
// as dropped_after_shutdown.
//
// Shutdown can also start inside a sink callback: a callback that calls
// exit() runs static destruction, and so the global core's shutdown, on the
// thread that is delivering (the dispatch thread, or a thread inside
// flush()). That thread holds the delivery lock further up its stack and
// never returns there, so the shutdown differs: the record whose callback is
// running is handed to the sinks that had not received it, everything still
// queued is delivered and every sink is flushed, all on that thread, except
// the sinks whose callbacks are on its stack. The sinks are then not
// destroyed, and the dispatch thread is left to end with the process instead
// of being joined. Destroying a Core you own from inside one of its own
// callbacks is undefined behaviour, as for any object destroyed while one of
// its member functions runs.
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
// record, the first exception thrown by each sink, a dispatch thread that
// could not be started.
//
// Calls. One Core calls its handler with one line at a time, in the order the
// lines were reported, never on two threads at once and never nested. The
// call happens on a thread that delivers records (the dispatch thread, or a
// thread inside flush()). A line reported while the handler is already
// running is queued, and the thread running the handler passes it on after
// the current call returns; so the handler may log, flush or replace itself,
// and a reporting thread does not wait for a handler busy elsewhere. No Core
// lock is held during the call except, for a sink's exception, the one that
// serialises delivery.
//
// A handler should not throw. If it does, the exception is caught, and the
// line and what was thrown are written to std::cerr; the process and the
// thread that delivers records go on. An empty handler discards the lines.
using DiagnosticHandler = std::function<void(std::string_view line)>;

// The handler a Core starts with: writes each line to std::cerr.
[[nodiscard]] DiagnosticHandler stderr_diagnostics();

struct QueueStatistics {
    uint64_t accepted = 0;  // queued for delivery (for logs, fatal records included)
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

// The records a BasicRecordBuilder makes: progress records come from
// ProgressHandle instead.
template <typename R>
concept BuiltRecord = RecordType<R> && !std::same_as<R, ProgressRecord>;

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
    // LumenError::NullSink if `sink` is null, and with LumenError::CoreShutDown
    // once the Core has begun shutting down (the sink is destroyed before
    // this returns).
    [[nodiscard]] std::expected<SinkId, std::error_code> add_sink(std::unique_ptr<Sink> sink,
                                                                  Predicate predicate = always());

    // Unregisters a sink and gives it back to the caller.
    //
    // When this returns successfully, the sink is not inside any callback and
    // Core will never call it again; records still queued are not delivered
    // to it. If one of its callbacks is running on another thread, this waits
    // for that callback to return (the one case where a call here waits for a
    // sink). Called from inside a callback of the same Core (directly, or
    // through a callback of another Core), it never waits: a sink whose
    // callback is running further up the calling thread's stack (the calling
    // sink itself, or the sink that raised a fatal record from its callback)
    // cannot be removed, and the call fails with LumenError::SinkBusy and
    // leaves it registered. Any other sink is removed at once.
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
    // itself. The same holds when the callback belongs to another Core that
    // this Core's delivery called into (a sink of A flushes B, a sink of B
    // flushes A). The records stay queued and are delivered when the current
    // pass continues. After shutdown it does nothing.
    //
    // Sinks of two Cores that flush each other from their callbacks on two
    // different threads at the same moment deadlock, as any two locks taken
    // in opposite orders do: each thread holds its own Core's delivery and
    // waits for the other's. Do not build such a cycle.
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
    enum class SourceStrings : uint8_t {
        Static,
        Copied
    };

    // The one way records enter: stamps, merges context, owns the strings,
    // queues. `tags_dropped` counts explicit tags the builder could not hold.
    void submit_(LogRecord&& record, uint32_t tags_dropped, SourceStrings source) noexcept;
    void submit_(MetricRecord&& record, uint32_t tags_dropped) noexcept;
    void submit_(ProgressRecord&& record, uint32_t tags_dropped) noexcept;
    // For a Fatal record: delivers everything queued, then this record, flushes
    // the sinks and terminates (see "Fatal records" above).
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
