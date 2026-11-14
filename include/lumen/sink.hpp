#ifndef LUMEN_SINK_HPP
#define LUMEN_SINK_HPP

#include "lumen/record.hpp"

// Sink: where records end up. Register one with Core::add_sink.
//
// Threading. Core calls a sink's on_log, on_metric, on_progress and flush on
// one thread at a time and never concurrently with each other (see
// lumen/core.hpp): usually the Core's dispatch thread, but flush() and the
// records it delivers run on the thread that called Core::flush().
//
// on_* must not block. The dispatch thread serves every sink of a Core, so a
// callback that waits for a terminal, a disk or a lock held by a slow thread
// delays every other sink and, once the queues fill, makes the Core drop
// records. A sink that does I/O hands the work to a thread of its own. The
// sinks shipped with Lumen do, and their on_* never waits for I/O:
// TerminalSink and FileSink format the record on the calling thread and
// append the text to a bounded queue that their writer thread empties
// (lumen/writer_statistics.hpp states what happens when that queue is full);
// DashboardSink updates its model under a mutex that its drawing thread holds
// only to copy the model. Their diagnostics (a full queue, a failed write)
// are reported by those threads, never by on_*, so a diagnostic handler that
// writes to a blocked standard error does not block the dispatch thread
// either.
//
// flush() is the opposite: it is where a sink may wait. It returns when what
// the sink received before the call has reached its destination (for the
// shipped sinks: written and the stream flushed; not fsync'ed). After a sink
// received a Fatal record, Core flushes it and terminates the process; the
// shipped sinks then wait at most a bounded time (fatal_flush_timeout in
// lumen/writer_statistics.hpp; DashboardSink about one second).
//
// Exceptions. A sink should not throw. One that does is contained by Core
// (counted and reported, see Core::sink_exception_count), and the record is
// lost for that sink only.
//
// Records. Each callback receives a record that owns its strings (see
// lumen/record.hpp); a sink may copy it and keep the copy.
//
// The shipped sinks, each in its own header:
//   lumen/terminal_sink.hpp   lines on a terminal or any std::ostream
//   lumen/file_sink.hpp       text, key=value or JSON lines in a rotating file
//   lumen/dashboard_sink.hpp  a live full-screen view (LUMEN_ENABLE_DASHBOARD)
//   NullSink, below           discards everything

namespace lumen {

class Sink {
public:
    Sink() = default;
    virtual ~Sink() = default;
    Sink(const Sink&) = delete;
    Sink& operator=(const Sink&) = delete;
    Sink(Sink&&) = delete;
    Sink& operator=(Sink&&) = delete;

    virtual void on_log(const LogRecord& /*record*/) {}
    virtual void on_metric(const MetricRecord& /*record*/) {}
    virtual void on_progress(const ProgressRecord& /*record*/) {}
    virtual void flush() = 0;
};

// Accepts every record and does nothing with it.
class NullSink final : public Sink {
public:
    void flush() override {}
};

}  // namespace lumen

#endif
