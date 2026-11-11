#ifndef LUMEN_WRITER_STATISTICS_HPP
#define LUMEN_WRITER_STATISTICS_HPP

#include <chrono>
#include <cstdint>

// Every sink shipped with Lumen that writes lines (TerminalSink, FileSink)
// does so through a writer thread of its own:
//
//   on_* (dispatch thread)                 writer thread
//   format the record into one line   ->   queue   ->   write a batch of lines,
//   append it to the queue                              flush the stream,
//                                                       report diagnostics
//
// on_* never waits for the destination or for the diagnostic handler: it
// takes a mutex that the writer thread holds only to swap the queue out, and
// returns. A destination or a diagnostic handler that blocks (standard error
// on a pipe nobody reads, a terminal stopped with Ctrl-S) stalls only the
// writer thread of that sink.
//
// The queue is bounded by the sink's Config::max_queued_lines. When it is
// full, on_* does not wait: the new line is dropped and counted in
// lines_dropped. The writer thread reports the first drop once through the
// sink's diagnostic handler, as soon as it is free; later drops are only
// counted.
//
// A write that fails (a closed pipe, a full disk) is not retried: the lines
// of that write are counted in lines_failed, and the first failure is
// reported once through the diagnostic handler. Later failures are only
// counted. Writing continues with the next lines.
//
// The sink's diagnostic handler is called on the writer thread only, never
// concurrently with itself.
//
// flush() returns when every line queued before it has been written (or has
// failed) and the stream flushed, and the first drop, if there was one, has
// been reported. It waits as long as the destination blocks, with one
// exception: once the sink has received a LogLevel::Fatal record, after
// which Core flushes and terminates the process, flush() waits at most
// fatal_flush_timeout and the lines not written by then are lost with the
// process.
//
// After the sink's flush() returns, and while nothing else is delivered to
// it, lines_queued == lines_written + lines_failed exactly (unless the
// fatal timeout cut flush() short). At any other time each counter is a
// snapshot taken at a slightly different moment.

namespace lumen {

struct WriterStatistics {
    uint64_t lines_queued = 0;   // accepted into the queue by on_*
    uint64_t lines_written = 0;  // handed to the destination without an error
    uint64_t lines_dropped = 0;  // refused because the queue was full
    uint64_t lines_failed = 0;   // lost to a failed write
};

// How long flush() waits for the destination after the sink received a Fatal
// record (see above).
inline constexpr std::chrono::milliseconds fatal_flush_timeout{2000};

}  // namespace lumen

#endif
