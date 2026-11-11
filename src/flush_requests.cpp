#include "flush_requests.hpp"

#include <memory>

namespace lumen::detail {

uint64_t FlushRequests::request() {
    const std::scoped_lock lock(mutex_);
    const uint64_t ticket = requested_.load(std::memory_order_relaxed) + 1;
    requested_.store(ticket, std::memory_order_release);
    return ticket;
}

bool FlushRequests::pending() const noexcept {
    return requested_.load(std::memory_order_acquire) > completed_.load(std::memory_order_acquire);
}

uint64_t FlushRequests::latest() const noexcept {
    return pending() ? requested_.load(std::memory_order_acquire) : 0;
}

void FlushRequests::complete(uint64_t ticket) {
    {
        const std::scoped_lock lock(mutex_);
        if (ticket > completed_.load(std::memory_order_relaxed)) {
            completed_.store(ticket, std::memory_order_release);
        }
    }
    changed_.notify_all();
}

void FlushRequests::complete_all() {
    {
        const std::scoped_lock lock(mutex_);
        completed_.store(requested_.load(std::memory_order_relaxed), std::memory_order_release);
    }
    changed_.notify_all();
}

void FlushRequests::close() {
    {
        const std::scoped_lock lock(mutex_);
        closed_ = true;
    }
    changed_.notify_all();
}

std::unique_lock<std::mutex> FlushRequests::lock_for_fork() {
    return std::unique_lock(mutex_);
}

void FlushRequests::reset_in_fork_child() noexcept {
    // A parent thread may have been waiting on the condition variable; its
    // state names waiters that do not exist here.
    std::construct_at(&changed_);
    completed_.store(requested_.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

}  // namespace lumen::detail
