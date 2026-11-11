#include "line_writer.hpp"

#include <algorithm>
#include <exception>
#include <format>
#include <new>
#include <utility>

namespace lumen::detail {

LineWriter::LineWriter(std::string sink_name, std::size_t max_queued_lines, BatchWriter write,
                       DiagnosticHandler diagnostics)
    : sink_name_(std::move(sink_name))
    , max_queued_lines_(max_queued_lines)
    , write_(std::move(write))
    , diagnostics_(std::move(diagnostics))
    , thread_([this] { run_(); }) {}

LineWriter::~LineWriter() {
    {
        const std::scoped_lock lock(mutex_);
        stopping_ = true;
    }
    work_cv_.notify_one();
    thread_.join();
}

void LineWriter::enqueue(std::string line) noexcept {
    bool dropped = false;
    {
        const std::scoped_lock lock(mutex_);
        if (queue_.size() >= max_queued_lines_) {
            dropped = true;
        } else {
            try {
                queue_.push_back(std::move(line));
                ++lines_accepted_;
                lines_queued_.fetch_add(1, std::memory_order_relaxed);
            } catch (const std::bad_alloc&) {
                dropped = true;
            }
        }
    }
    if (dropped) {
        lines_dropped_.fetch_add(1, std::memory_order_relaxed);
        report_first_drop_();
        return;
    }
    work_cv_.notify_one();
}

void LineWriter::wait_until_written() {
    std::unique_lock lock(mutex_);
    const uint64_t target = lines_accepted_;
    written_cv_.wait(lock, [&] { return lines_completed_ >= target; });
}

WriterStatistics LineWriter::statistics() const noexcept {
    return {
        .lines_queued = lines_queued_.load(std::memory_order_relaxed),
        .lines_written = lines_written_.load(std::memory_order_relaxed),
        .lines_dropped = lines_dropped_.load(std::memory_order_relaxed),
        .lines_failed = lines_failed_.load(std::memory_order_relaxed),
    };
}

void LineWriter::run_() noexcept {
    std::vector<std::string> batch;
    while (true) {
        {
            std::unique_lock lock(mutex_);
            work_cv_.wait(lock, [&] { return !queue_.empty() || stopping_; });
            if (queue_.empty()) {
                return;  // stopping, and everything queued has been written
            }
            batch.clear();
            batch.swap(queue_);
        }
        const BatchOutcome outcome = write_batch_(batch);
        const uint64_t failed = std::min<uint64_t>(outcome.failed_lines, batch.size());
        lines_written_.fetch_add(batch.size() - failed, std::memory_order_relaxed);
        lines_failed_.fetch_add(failed, std::memory_order_relaxed);
        if (failed > 0 || !outcome.failure.empty()) {
            report_first_failure_(outcome.failure.empty() ? "no detail" : outcome.failure);
        }
        {
            const std::scoped_lock lock(mutex_);
            lines_completed_ += batch.size();
        }
        written_cv_.notify_all();
    }
}

BatchOutcome LineWriter::write_batch_(std::span<const std::string> lines) noexcept {
    try {
        return write_(lines);
    } catch (const std::exception& e) {
        return {.failed_lines = lines.size(), .failure = e.what()};
    } catch (...) {
        return {.failed_lines = lines.size(), .failure = "unknown exception"};
    }
}

void LineWriter::report_first_drop_() noexcept {
    if (drop_reported_.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    try {
        report_(
            std::format("lumen: {}: output queue full ({} lines); lines are dropped while it "
                        "stays full and only counted (statistics().lines_dropped)",
                        sink_name_, max_queued_lines_));
    } catch (const std::bad_alloc&) {
        return;  // no memory for the message; the drop is still counted
    }
}

void LineWriter::report_first_failure_(std::string_view failure) noexcept {
    if (failure_reported_.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    try {
        report_(
            std::format("lumen: {}: write failed ({}); later failures are only counted "
                        "(statistics().lines_failed)",
                        sink_name_, failure));
    } catch (const std::bad_alloc&) {
        return;  // no memory for the message; the failure is still counted
    }
}

void LineWriter::report_(std::string_view text) noexcept {
    const std::scoped_lock lock(diagnostics_mutex_);
    if (!diagnostics_) {
        return;  // an empty handler means: discard diagnostics
    }
    try {
        diagnostics_(text);
    } catch (...) {
        // Nowhere is left to report a failing diagnostic handler; the event
        // itself is still in statistics().
        return;
    }
}

}  // namespace lumen::detail
