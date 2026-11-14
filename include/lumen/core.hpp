#ifndef LUMEN_CORE_HPP
#define LUMEN_CORE_HPP

#include <chrono>
#include <concepts>
#include <cstdint>
#include <expected>
#include <memory>
#include <string_view>
#include <system_error>

#include "lumen/diagnostics.hpp"
#include "lumen/predicate.hpp"
#include "lumen/record.hpp"
#include "lumen/sink.hpp"

// Core: the broker between producers and sinks.
//
// Flow. A producer (a logging macro, lumen::metric, a ProgressHandle) builds
// a record on its own thread and submits it to a Core. Submission stamps the
// record (timestamp, thread id), merges the ambient context
// (lumen/context.hpp), copies every string into the record's own storage and
// pushes the record into a bounded lock-free queue, one per record type. A
// dispatch thread owned by the Core pops records and calls the sinks whose
// predicate matches.
//
// Cost of a statement, on the producing thread, once that thread has logged
// before (its first statement creates its context):
//   - Heap allocations: one, the block that holds all of the record's
//     strings, for any log statement, metric or progress update, with or
//     without tags and ambient context, and also for a record that its full
//     queue then refuses. Each string that does not fit in the builder's
//     512-byte buffer adds two (lumen/detail/string_scratch.hpp). Nothing
//     else allocates; the first record after the process tags changed only
//     shares the new table.
//   - Locks: none of Lumen's own, except that the first record after the
//     process tags changed takes the process table's mutex to read them (so
//     does every record of a thread whose context was destroyed, see
//     lumen/context.hpp, and every record of a Core without a dispatch
//     thread, see "Threads"). The memory allocator has locks of its own,
//     which the dispatch thread also takes when it frees a delivered
//     record's block.
//   - System calls: none of Lumen's own unless the dispatch thread is asleep
//     (then one, to wake it). The allocator makes some when it grows a
//     thread's heap: on Linux with glibc, about one per 90 000 records while
//     the dispatcher keeps up, and about one per 450 records while producers
//     outpace it and the queue stays full.
//   - Time: tests/bench_emit.cpp measures it.
//
// Threads.
//   - Producers: any number of threads; see "Cost of a statement".
//   - Sink callbacks (on_log, on_metric, on_progress, flush): one thread at a
//     time. Usually the dispatch thread; flush() delivers on its caller's
//     thread instead (flush_for asks the dispatch thread), and fatal records
//     and shutdown deliver on the thread that raises or runs them. A sink is
//     never called concurrently with itself or with another sink of the same
//     Core.
//   - If the dispatch thread cannot be started (the system refuses a new
//     thread), the constructor does not throw. The Core reports it through
//     the diagnostic handler and then delivers each record on the thread
//     that submits it, right after queuing it, under the same lock that
//     serialises delivery (so submission takes that lock). A record a sink
//     submits from inside its callback waits for the next submission or
//     flush(), and flush_for delivers on its caller's thread as flush() does,
//     without its time limit and without the cycle check. Everything else is
//     unchanged.
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
// Out of memory. Lumen does not turn a failed allocation into an error
// code. Where it happens in submission (a record's string block, a fatal
// record's place in its mailbox), on the dispatch thread, or while reporting
// a sink's exception, std::bad_alloc ends the process through
// std::terminate(). Elsewhere (the constructor, add_sink,
// set_diagnostic_handler, flush) it may propagate to the caller.
//
// Fatal records (LogLevel::Fatal, see BasicRecordBuilder) are not queued: the
// thread that raises one delivers, on its own stack, everything queued
// before it, then the fatal record, flushes every sink and ends the process
// with SIGABRT: by std::abort() if the terminate handler is still the one
// installed before main (the C++ library's default, which would first write
// a message to standard error and never return when standard error blocks),
// otherwise by std::terminate(), which calls the program's handler. If the
// delivery lock is held by a thread that makes no progress (a sink callback
// that does not return) for the shutdown timeout, the fatal record is not
// delivered: this is reported and the process ends all the same. Raised from
// inside a sink callback, the record whose
// callback is running is first handed to the sinks that had not received it
// yet; the sink whose callback raised the fatal record receives nothing more
// (it is in the middle of a call). When several threads raise fatal records
// at once, one thread delivers them all, in the order they were submitted,
// and terminates the process: after the last fatal record it knows of, it
// waits 20 ms for others, and a fatal record submitted before that wait ends
// is delivered too. One submitted later is not.
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
//
// Shutdown that gives up. Shutdown waits for the dispatch thread to return,
// and for the delivery lock, only while delivery makes progress: if no
// record is delivered and no sink flushed for the shutdown timeout
// (set_shutdown_timeout, default_shutdown_timeout), a callback does not
// return, and shutdown abandons the Core instead of waiting forever. It
// reports through the diagnostic handler how many records were still queued
// (statistics().<kind>.queued); those are not delivered, the sinks are
// neither flushed nor destroyed, the dispatch thread is left to end with the
// process, and flush() calls waiting on another thread return
// (LumenError::CoreShutDown). A Core you own that is abandoned in its
// destructor is not freed: its thread may still return from the callback and
// use it. A callback on the shutting-down thread itself (a sink's flush() in
// the final pass) is not bounded.
//
// fork() (Linux and macOS). The library registers pthread_atfork handlers.
//   - Records queued in the parent when it forks are delivered by the
//     parent; the child's copies are discarded and not counted again. Fatal
//     records and diagnostic lines pending in the parent are the parent's.
//   - In the child, a Core goes on: its locks are usable, and its dispatch
//     thread is started again by the first record, flush or flush_for there.
//     The shipped sinks start their writer threads again likewise; lines
//     queued in the parent's sink are written by the parent. A DashboardSink
//     draws only in the parent: in the child it takes records and does
//     nothing else (the terminal belongs to the parent). Statistics continue
//     from the parent's values.
//   - Sinks of your own that hold threads or locks must handle fork
//     themselves.
//   - A child that ends with _exit() (or exec) loses the records it queued
//     and the dispatch thread had not delivered, as with any buffered
//     output; exit() and a return from main shut the global core down as in
//     any process.
//   - fork() from inside a sink callback: the child goes on with the
//     delivery that callback belongs to on the same thread, so the record
//     that callback received is offered to the sinks after it in both
//     processes.
// What this relies on. Strictly, the child of a multithreaded process may
// only call async-signal-safe functions until it calls exec. Lumen's child
// handler only stores to memory, re-initialises locks and condition
// variables that no thread of the child uses, and frees memory; the threads
// are started later, at the first use. That freeing memory and starting
// threads work in the child is what glibc and macOS provide, and what every
// language runtime that forks (Python's multiprocessing among them) relies
// on too. Locks of other libraries held by other threads at the fork are
// their business.

namespace lumen {

using SinkId = uint64_t;

// A Core's DiagnosticHandler (lumen/diagnostics.hpp) receives one line about
// each problem the Core cannot return to anyone: the first record refused by
// a full queue, the first tag left out of a full record, the first exception
// thrown by each sink, a dispatch thread that could not be started, a
// shutdown that gave up waiting for a sink callback. A Core starts with
// stderr_diagnostics().
//
// Calls. One Core calls its handler with one line at a time, in the order the
// lines were reported, never on two threads at once and never nested. The
// call happens on the thread that found the problem: one that delivers
// records (the dispatch thread, a thread inside flush(), a thread that
// raised a fatal record), the thread constructing the Core (a dispatch
// thread that could not be started; in a fork child, the thread whose first
// use starts it again), or the thread shutting the Core down (the losses of
// the final pass, a shutdown that gave up). A line reported while the
// handler is already
// running is queued, and the thread running the handler passes it on after
// the current call returns; so the handler may log, flush or replace itself,
// and a reporting thread does not wait for a handler busy elsewhere. No Core
// lock is held during the call except, for a sink's exception, the one that
// serialises delivery.
//
// A handler should not throw. If it does, the exception is caught, and the
// line and what was thrown are written to std::cerr; the process and the
// thread that delivers records go on.

// How long shutdown and a fatal record wait for a thread that delivers
// records and makes no progress (see "Shutdown that gives up").
inline constexpr std::chrono::milliseconds default_shutdown_timeout{5000};

struct QueueStatistics {
    uint64_t accepted = 0;                // queued for delivery (for logs, fatal records included)
    uint64_t dropped_queue_full = 0;      // refused: the queue was full
    uint64_t dropped_after_shutdown = 0;  // submitted after the Core shut down
    uint64_t queued = 0;                  // accepted and not yet handed to the sinks
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

    // Both deliver every record submitted before the call, then call flush()
    // on every sink, and return when all of that is done.
    //
    // flush() does the work on the calling thread (serialised with the
    // dispatch thread), and waits without a limit for the delivery lock and
    // for the sinks. flush_for() has the dispatch thread do it and waits at
    // most `timeout`, so a caller that stops waiting is never left inside a
    // sink callback (without a dispatch thread, see Threads above, it works
    // as flush() does).
    //
    // flush_for reports why it did not flush; flush() returns in the same
    // cases, without saying why:
    //   - LumenError::FlushTimedOut (flush_for only): the time ran out (a
    //     slow or blocked sink). The flush is still made later; giving up
    //     loses nothing.
    //   - LumenError::SinkBusy, at once, from inside a sink callback of this
    //     Core (directly, or through a callback of another Core): the
    //     calling thread is the one delivering and cannot wait for itself.
    //     The records stay queued and are delivered when the current pass
    //     continues.
    //   - LumenError::SinkBusy, within about 5 ms, when waiting would never
    //     end because of a cycle between Cores: a callback of Core A flushes
    //     Core B while B's delivering thread, in a callback of its own,
    //     flushes A (directly, or through further Cores). Each flush in the
    //     cycle may fail this way; none waits forever.
    //   - LumenError::CoreShutDown: the Core has shut down, or gave up its
    //     shutdown while this waited.
    [[nodiscard]] std::expected<void, std::error_code> flush_for(std::chrono::milliseconds timeout);
    void flush();

    [[nodiscard]] CoreStatistics statistics() const noexcept;

    // Replaces the diagnostic handler; an empty one silences diagnostics.
    void set_diagnostic_handler(DiagnosticHandler handler);

    // How long shutdown and a fatal record wait for a delivering thread that
    // makes no progress before giving up (see "Shutdown that gives up");
    // default_shutdown_timeout to start with. Any thread.
    void set_shutdown_timeout(std::chrono::milliseconds timeout) noexcept;

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
