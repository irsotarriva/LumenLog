#include "fatal_records.hpp"

#include <chrono>
#include <thread>
#include <utility>

namespace lumen::detail {

namespace {

// How long the delivering thread waits, after the last fatal record it
// knows of, for one that another thread raises at about the same time (for
// instance every worker failing on the same input). Delays termination by
// this much once; a record announced meanwhile restarts the wait.
constexpr std::chrono::milliseconds late_fatal_grace{20};

}  // namespace

void FatalRecords::announce() noexcept {
    announced_.fetch_add(1, std::memory_order_acq_rel);
}

void FatalRecords::post(LogRecord&& record) {
    const std::scoped_lock lock(mutex_);
    mailbox_.push_back(std::move(record));
}

void FatalRecords::deliver_all(Delivery& delivery) {
    delivery.finish_in_flight();
    while (true) {
        deliver_announced_(delivery);
        std::this_thread::sleep_for(late_fatal_grace);
        if (delivered_ == announced_.load(std::memory_order_acquire)) {
            break;
        }
    }
    delivery.flush_sinks();
}

void FatalRecords::deliver_announced_(Delivery& delivery) {
    while (true) {
        delivery.deliver_everything_queued();
        for (const LogRecord& fatal : take_()) {
            delivery.dispatch(fatal);
            ++delivered_;
        }
        if (delivered_ == announced_.load(std::memory_order_acquire)) {
            return;
        }
        // Another thread has announced a fatal record and is posting it.
        std::this_thread::yield();
    }
}

std::unique_lock<std::mutex> FatalRecords::lock_for_fork() {
    return std::unique_lock(mutex_);
}

void FatalRecords::reset_in_fork_child() noexcept {
    mailbox_.clear();
    // The child delivers from here on as if every earlier fatal record had
    // been delivered; statistics() still counts them (announced()).
    delivered_ = announced_.load(std::memory_order_relaxed);
}

uint64_t FatalRecords::announced() const noexcept {
    return announced_.load(std::memory_order_relaxed);
}

std::vector<LogRecord> FatalRecords::take_() {
    const std::scoped_lock lock(mutex_);
    return std::exchange(mailbox_, {});
}

}  // namespace lumen::detail
