#include "wakeup.hpp"

#include <chrono>
#include <cstdint>
#include <thread>

namespace lumen::detail {

namespace {

// See Wakeup::wait_for_work().
constexpr std::chrono::microseconds idle_check_interval{100};
constexpr uint32_t idle_check_count = 5;

}  // namespace

void Wakeup::wake_if_sleeping() noexcept {
    if (!sleeping_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    {
        // Empty critical section: the dispatcher tests `sleeping_` and starts
        // waiting under mutex_, so after this the notify cannot fall between
        // its test and its wait.
        const std::scoped_lock lock(mutex_);
    }
    woken_.notify_one();
}

bool Wakeup::wait_for_work(const RecordQueues& queues) {
    if (stopping_.load(std::memory_order_acquire)) {
        return false;
    }
    if (found_work_before_sleeping_(queues)) {
        return true;
    }
    block_until_woken_();
    return !stopping_.load(std::memory_order_relaxed);
}

void Wakeup::stop() noexcept {
    {
        const std::scoped_lock lock(mutex_);
        stopping_.store(true, std::memory_order_release);
    }
    woken_.notify_one();
}

bool Wakeup::found_work_before_sleeping_(const RecordQueues& queues) {
    for (uint32_t check = 0; check < idle_check_count && !queues.has_pending(); ++check) {
        std::this_thread::sleep_for(idle_check_interval);
    }
    if (queues.has_pending()) {
        return true;
    }
    // The previous value is false: only this thread sets the flag.
    static_cast<void>(sleeping_.exchange(true, std::memory_order_acq_rel));
    if (queues.has_pending()) {
        sleeping_.store(false, std::memory_order_relaxed);
        return true;
    }
    return false;
}

void Wakeup::block_until_woken_() {
    std::unique_lock lock(mutex_);
    woken_.wait(lock, [this] {
        return !sleeping_.load(std::memory_order_relaxed) ||
               stopping_.load(std::memory_order_relaxed);
    });
    sleeping_.store(false, std::memory_order_relaxed);
}

}  // namespace lumen::detail
