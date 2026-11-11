#ifndef LUMEN_SRC_LINE_WRITER_HPP
#define LUMEN_SRC_LINE_WRITER_HPP

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "lumen/core.hpp"
#include "lumen/writer_statistics.hpp"

// The writer thread of the line-writing sinks: a bounded queue of lines, and
// a thread that hands them to the destination in batches. The behaviour it
// implements is documented for users in lumen/writer_statistics.hpp.

namespace lumen::detail {

// What happened to one batch: how many of its lines were lost, and a short
// description of the failures. A failure can come without lost lines (a
// rotation step that failed while writing went on in the open file).
struct BatchOutcome {
    uint64_t failed_lines = 0;
    std::string failure;
};

// Writes `lines` to the destination and flushes it. Runs on the writer thread
// only. An exception it throws is caught and counts the whole batch as failed.
using BatchWriter = std::function<BatchOutcome(std::span<const std::string> lines)>;

class LineWriter {
    // Only create() can make the key, so a LineWriter always has its thread.
    class ConstructionKey {
        friend class LineWriter;
        ConstructionKey() = default;
    };

public:
    // Starts the writer thread. `sink_name` prefixes diagnostic lines
    // ("lumen: <sink_name>: ..."). `max_queued_lines` must be at least 1.
    // `diagnostics` is called on the writer thread only. Fails with the
    // error std::thread reports when the thread cannot be started
    // (std::errc::resource_unavailable_try_again when the system is out of
    // threads), or std::errc::not_enough_memory.
    [[nodiscard]] static std::expected<std::unique_ptr<LineWriter>, std::error_code> create(
        std::string sink_name, std::size_t max_queued_lines, BatchWriter write,
        DiagnosticHandler diagnostics);

    LineWriter(ConstructionKey key, std::string sink_name, std::size_t max_queued_lines,
               BatchWriter write, DiagnosticHandler diagnostics);
    // Writes every queued line, then stops the thread.
    ~LineWriter();
    LineWriter(const LineWriter&) = delete;
    LineWriter& operator=(const LineWriter&) = delete;
    LineWriter(LineWriter&&) = delete;
    LineWriter& operator=(LineWriter&&) = delete;

    // Queues `line` without waiting for the destination or for a diagnostic
    // handler. When the queue already holds max_queued_lines lines, drops
    // `line`, counts it, and leaves the report of the first drop to the
    // writer thread.
    void enqueue(std::string line) noexcept;

    // The process is about to terminate (the sink received a Fatal record):
    // from now on wait_until_written gives up after fatal_flush_timeout.
    void expect_termination() noexcept;

    // Returns when every line queued before the call has been handed to the
    // destination (written or failed) and a pending drop report has been
    // made; after expect_termination(), at the latest after
    // fatal_flush_timeout.
    void wait_until_written();

    [[nodiscard]] WriterStatistics statistics() const noexcept;

private:
    // Starts the thread; create() calls it once.
    [[nodiscard]] std::expected<void, std::error_code> start_() noexcept;
    void run_() noexcept;
    // Waits for lines or a drop report and swaps the queue into `batch`.
    // Returns false when the thread should end (stopping, nothing left).
    [[nodiscard]] bool take_work_(std::vector<std::string>& batch, bool& report_drop);
    // Writes the batch and updates the counters; reports the first failure.
    void write_and_count_(std::span<const std::string> batch) noexcept;
    [[nodiscard]] BatchOutcome write_batch_(std::span<const std::string> lines) noexcept;
    void report_drop_() noexcept;
    // Reports the first failure only.
    void report_first_failure_(std::string_view failure) noexcept;
    void report_(std::string_view text) noexcept;

    const std::string sink_name_;
    const std::size_t max_queued_lines_;
    const BatchWriter write_;
    const DiagnosticHandler diagnostics_;  // called by the writer thread only
    bool failure_reported_ = false;        // used by the writer thread only

    std::atomic<uint64_t> lines_queued_{0};
    std::atomic<uint64_t> lines_written_{0};
    std::atomic<uint64_t> lines_dropped_{0};
    std::atomic<uint64_t> lines_failed_{0};

    std::mutex mutex_;
    std::condition_variable work_cv_;     // waits on mutex_: lines queued, a report due, stopping
    std::condition_variable written_cv_;  // waits on mutex_: lines_completed_ advanced
    std::vector<std::string> queue_;      // guarded by mutex_
    uint64_t lines_accepted_ = 0;         // guarded by mutex_: lines ever queued
    uint64_t lines_completed_ = 0;        // guarded by mutex_: lines written or failed
    bool drop_seen_ = false;              // guarded by mutex_: a line was ever dropped
    bool drop_report_pending_ = false;    // guarded by mutex_: the first drop is not reported yet
    bool termination_expected_ = false;   // guarded by mutex_
    bool stopping_ = false;               // guarded by mutex_

    // std::thread rather than std::jthread: Apple's libc++ has no jthread.
    // Started by start_() once every member above exists.
    std::thread thread_;
};

}  // namespace lumen::detail

#endif
