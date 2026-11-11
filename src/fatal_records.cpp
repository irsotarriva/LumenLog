#include "fatal_records.hpp"

#include <thread>
#include <utility>

namespace lumen::detail {

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
        delivery.deliver_everything_queued();
        for (const LogRecord& fatal : take_()) {
            delivery.dispatch(fatal);
            ++delivered_;
        }
        if (delivered_ == announced_.load(std::memory_order_acquire)) {
            break;
        }
        // Another thread has announced a fatal record and is posting it.
        std::this_thread::yield();
    }
    delivery.flush_sinks();
}

uint64_t FatalRecords::announced() const noexcept {
    return announced_.load(std::memory_order_relaxed);
}

std::vector<LogRecord> FatalRecords::take_() {
    const std::scoped_lock lock(mutex_);
    return std::exchange(mailbox_, {});
}

}  // namespace lumen::detail
