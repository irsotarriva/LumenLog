#ifndef LUMEN_SRC_LINE_WRITER_HPP
#define LUMEN_SRC_LINE_WRITER_HPP

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "lumen/core.hpp"
#include "lumen/writer_statistics.hpp"

// The writer thread of the line-writing sinks: a bounded queue of lines, and
// a thread that hands them to the destination in batches. The behaviour it
// implements is documented for users in lumen/writer_statistics.hpp.

namespace lumen::detail {

// What happened to one batch: how many of its lines were lost, and a short
// description of the first failure. A failure can come without lost lines
// (a rotation step that failed while writing went on in the open file).
struct BatchOutcome {
    uint64_t failed_lines = 0;
    std::string failure;
};

// Writes `lines` to the destination and flushes it. Runs on the writer thread
// only. An exception it throws is caught and counts the whole batch as failed.
using BatchWriter = std::function<BatchOutcome(std::span<const std::string> lines)>;

class LineWriter {
public:
    // `sink_name` prefixes diagnostic lines ("lumen: <sink_name>: ...").
    // `max_queued_lines` must be at least 1.
    LineWriter(std::string sink_name, std::size_t max_queued_lines, BatchWriter write,
               DiagnosticHandler diagnostics);
    // Writes every queued line, then stops the thread.
    ~LineWriter();
    LineWriter(const LineWriter&) = delete;
    LineWriter& operator=(const LineWriter&) = delete;
    LineWriter(LineWriter&&) = delete;
    LineWriter& operator=(LineWriter&&) = delete;

    // Queues `line` without waiting for the destination. When the queue
    // already holds max_queued_lines lines, drops `line` and counts it.
    void enqueue(std::string line) noexcept;

    // Returns when every line queued before the call has been handed to the
    // destination (written or failed).
    void wait_until_written();

    [[nodiscard]] WriterStatistics statistics() const noexcept;

private:
    void run_() noexcept;
    [[nodiscard]] BatchOutcome write_batch_(std::span<const std::string> lines) noexcept;
    // Each reports its event the first time only.
    void report_first_drop_() noexcept;
    void report_first_failure_(std::string_view failure) noexcept;
    void report_(std::string_view text) noexcept;

    const std::string sink_name_;
    const std::size_t max_queued_lines_;
    const BatchWriter write_;

    std::mutex diagnostics_mutex_;   // serialises calls of diagnostics_
    DiagnosticHandler diagnostics_;  // guarded by diagnostics_mutex_
    std::atomic<bool> drop_reported_{false};
    std::atomic<bool> failure_reported_{false};

    std::atomic<uint64_t> lines_queued_{0};
    std::atomic<uint64_t> lines_written_{0};
    std::atomic<uint64_t> lines_dropped_{0};
    std::atomic<uint64_t> lines_failed_{0};

    std::mutex mutex_;
    std::condition_variable work_cv_;     // waits on mutex_: lines queued or stopping
    std::condition_variable written_cv_;  // waits on mutex_: lines_completed_ advanced
    std::vector<std::string> queue_;      // guarded by mutex_
    uint64_t lines_accepted_ = 0;         // guarded by mutex_: lines ever queued
    uint64_t lines_completed_ = 0;        // guarded by mutex_: lines written or failed
    bool stopping_ = false;               // guarded by mutex_

    // std::thread rather than std::jthread: Apple's libc++ has no jthread.
    // Declared last, so that it starts after every member above exists.
    std::thread thread_;
};

}  // namespace lumen::detail

#endif
