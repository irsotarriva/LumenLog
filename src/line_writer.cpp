#include "line_writer.hpp"

#include "portable_error.hpp"

#include <algorithm>
#include <exception>
#include <format>
#include <memory>
#include <new>
#include <optional>
#include <utility>

namespace lumen::detail {

std::expected<std::unique_ptr<LineWriter>, std::error_code> LineWriter::create(
    std::string sink_name, std::size_t max_queued_lines, BatchWriter write,
    DiagnosticHandler diagnostics) {
    auto writer =
        std::make_unique<LineWriter>(ConstructionKey{}, std::move(sink_name), max_queued_lines,
                                     std::move(write), std::move(diagnostics));
    if (const std::expected<void, std::error_code> started = writer->start_(); !started) {
        return std::unexpected(started.error());
    }
    return writer;
}

LineWriter::LineWriter(ConstructionKey /*key*/, std::string sink_name, std::size_t max_queued_lines,
                       BatchWriter write, DiagnosticHandler diagnostics)
    : sink_name_(std::move(sink_name))
    , max_queued_lines_(max_queued_lines)
    , write_(std::move(write))
    , diagnostics_(std::move(diagnostics)) {}

LineWriter::~LineWriter() {
    {
        const std::scoped_lock lock(mutex_);
        stopping_ = true;
    }
    work_cv_.notify_one();
    if (thread_.joinable()) {
        thread_.join();
    }
}

std::expected<void, std::error_code> LineWriter::start_() noexcept {
    try {
        thread_ = std::thread([this] { run_(); });
    } catch (const std::system_error& e) {
        return std::unexpected(portable_error(e.code()));
    } catch (const std::bad_alloc&) {
        return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
    }
    return {};
}

void LineWriter::enqueue(std::string line) noexcept {
    bool accepted = false;
    bool report_due = false;
    std::optional<std::error_code> restart_failed;
    {
        const std::scoped_lock lock(mutex_);
        if (restart_due_) {
            const std::expected<void, std::error_code> started = start_();
            restart_due_ = !started;
            if (!started) {
                restart_failed = started.error();
            }
        }
        if (!restart_failed && queue_.size() < max_queued_lines_) {
            try {
                queue_.push_back(std::move(line));
                ++lines_accepted_;
                lines_queued_.fetch_add(1, std::memory_order_relaxed);
                accepted = true;
            } catch (const std::bad_alloc&) {
                accepted = false;  // counted as a drop below
            }
        }
        if (!accepted && !drop_seen_) {
            drop_seen_ = true;
            drop_report_pending_ = true;
            report_due = true;
        }
    }
    if (!accepted) {
        lines_dropped_.fetch_add(1, std::memory_order_relaxed);
    }
    if (restart_failed) {
        report_restart_failure_(*restart_failed);
        return;
    }
    if (accepted || report_due) {
        work_cv_.notify_one();
    }
}

// The writer thread that would report it does not exist.
void LineWriter::report_restart_failure_(std::error_code error) noexcept {
    if (restart_failure_reported_.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    try {
        report_(
            std::format("lumen: {}: cannot start the writer thread in the child process "
                        "({}); lines are dropped and counted (statistics().lines_dropped)",
                        sink_name_, error.message()));
    } catch (const std::bad_alloc&) {
        return;  // no memory for the message; the drops are still counted
    }
}

void LineWriter::prepare_fork_() {
    fork_lock_.emplace(mutex_);
}

// Runs in the child with mutex_ held by this thread (prepare_fork_). The
// condition variables may name waiters of the parent, and thread_ names the
// parent's writer thread; they are re-initialised, not destroyed.
void LineWriter::after_fork_in_child_() noexcept {
    queue_.clear();
    lines_completed_ = lines_accepted_;
    drop_report_pending_ = false;
    std::construct_at(&work_cv_);
    std::construct_at(&written_cv_);
    std::construct_at(&thread_);
    restart_due_ = !stopping_;
    fork_lock_.reset();
}

void LineWriter::expect_termination() noexcept {
    const std::scoped_lock lock(mutex_);
    termination_expected_ = true;
}

void LineWriter::wait_until_written() {
    std::unique_lock lock(mutex_);
    const uint64_t target = lines_accepted_;
    const auto done = [&] { return lines_completed_ >= target && !drop_report_pending_; };
    if (termination_expected_) {
        // Giving up is the point: the process terminates next, and a
        // destination that blocks must not keep it alive.
        static_cast<void>(written_cv_.wait_for(lock, fatal_flush_timeout, done));
        return;
    }
    written_cv_.wait(lock, done);
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
        bool report_drop = false;
        if (!take_work_(batch, report_drop)) {
            return;  // stopping, and everything queued has been written
        }
        if (report_drop) {
            report_drop_();
        }
        write_and_count_(batch);
        {
            const std::scoped_lock lock(mutex_);
            lines_completed_ += batch.size();
            if (report_drop) {
                drop_report_pending_ = false;
            }
        }
        written_cv_.notify_all();
    }
}

bool LineWriter::take_work_(std::vector<std::string>& batch, bool& report_drop) {
    std::unique_lock lock(mutex_);
    work_cv_.wait(lock, [&] { return !queue_.empty() || drop_report_pending_ || stopping_; });
    report_drop = drop_report_pending_;
    if (queue_.empty() && !report_drop) {
        return false;
    }
    batch.clear();
    batch.swap(queue_);
    return true;
}

void LineWriter::write_and_count_(std::span<const std::string> batch) noexcept {
    if (batch.empty()) {
        return;
    }
    const BatchOutcome outcome = write_batch_(batch);
    const uint64_t failed = std::min<uint64_t>(outcome.failed_lines, batch.size());
    lines_written_.fetch_add(batch.size() - failed, std::memory_order_relaxed);
    lines_failed_.fetch_add(failed, std::memory_order_relaxed);
    if (failed > 0 || !outcome.failure.empty()) {
        report_first_failure_(outcome.failure.empty() ? "no detail" : outcome.failure);
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

void LineWriter::report_drop_() noexcept {
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
    if (std::exchange(failure_reported_, true)) {
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
