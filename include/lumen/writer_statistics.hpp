#ifndef LUMEN_WRITER_STATISTICS_HPP
#define LUMEN_WRITER_STATISTICS_HPP

#include <cstdint>

// Every sink shipped with Lumen that writes lines (TerminalSink, FileSink)
// does so through a writer thread of its own:
//
//   on_* (dispatch thread)                 writer thread
//   format the record into one line   ->   queue   ->   write a batch of lines,
//   append it to the queue                              flush the stream
//
// The queue is bounded by the sink's Config::max_queued_lines. When it is
// full, on_* does not wait: the new line is dropped and counted in
// lines_dropped, and the first drop is reported once through the sink's
// diagnostic handler. Later drops are only counted.
//
// A write that fails (a closed pipe, a full disk) is not retried: the lines
// of that write are counted in lines_failed, and the first failure is
// reported once through the diagnostic handler. Later failures are only
// counted. Writing continues with the next lines.
//
// After the sink's flush() returns, and while nothing else is delivered to
// it, lines_queued == lines_written + lines_failed exactly. At any other time
// each counter is a snapshot taken at a slightly different moment.

namespace lumen {

struct WriterStatistics {
    uint64_t lines_queued = 0;   // accepted into the queue by on_*
    uint64_t lines_written = 0;  // handed to the destination without an error
    uint64_t lines_dropped = 0;  // refused because the queue was full
    uint64_t lines_failed = 0;   // lost to a failed write
};

}  // namespace lumen

#endif
