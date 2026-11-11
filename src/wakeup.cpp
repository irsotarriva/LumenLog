#include "wakeup.hpp"

#include <chrono>
#include <memory>
#include <thread>

namespace lumen::detail {

namespace {

// See Wakeup::wait_for_work().
constexpr std::chrono::microseconds idle_check_interval{100};

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

void Wakeup::stop() noexcept {
    {
        const std::scoped_lock lock(mutex_);
        stopping_.store(true, std::memory_order_release);
    }
    woken_.notify_one();
}

void Wakeup::pause_between_checks_() {
    std::this_thread::sleep_for(idle_check_interval);
}

std::unique_lock<std::mutex> Wakeup::lock_for_fork() {
    return std::unique_lock(mutex_);
}

void Wakeup::reset_in_fork_child() noexcept {
    std::construct_at(&woken_);
    sleeping_.store(false, std::memory_order_relaxed);
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
