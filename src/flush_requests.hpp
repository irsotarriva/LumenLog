#ifndef LUMEN_SRC_FLUSH_REQUESTS_HPP
#define LUMEN_SRC_FLUSH_REQUESTS_HPP

#include <atomic>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace lumen::detail {

// Flushes that callers of Core::flush and Core::flush_for ask the dispatch
// thread to make, so that a caller can stop waiting without being left
// inside a sink callback.
//
// A caller takes a ticket and waits until it is completed. The dispatch
// thread reads the latest ticket before it reads the queue heads, delivers
// up to those heads, flushes every sink and completes that ticket and every
// earlier one: the records submitted before a ticket was taken were queued
// before the heads were read.
class FlushRequests {
public:
    using Clock = std::chrono::steady_clock;

    enum class Outcome : uint8_t {
        Done,      // the ticket was completed
        TimedOut,  // the deadline passed first
        Closed,    // close() was called first
        Cycle      // the cycle check said that waiting would never end
    };

    // Caller side. Returns the ticket to wait for.
    [[nodiscard]] uint64_t request();

    // Waits until `ticket` is completed, `deadline` passes or close() is
    // called. If `check_cycle` is set, also wakes every cycle_check_interval
    // and gives up when `in_cycle()` is true.
    template <std::predicate F>
    [[nodiscard]] Outcome wait(uint64_t ticket, Clock::time_point deadline, bool check_cycle,
                               F in_cycle);

    // Dispatch thread side. Whether a ticket is waiting, and the latest one
    // (0 for none), read before the queue heads.
    [[nodiscard]] bool pending() const noexcept;
    [[nodiscard]] uint64_t latest() const noexcept;
    // Completes `ticket` and every earlier one.
    void complete(uint64_t ticket);

    // Shutdown: complete_all() after everything queued has been delivered
    // and the sinks flushed; close() when that will not happen.
    void complete_all();
    void close();

    // Fork handlers (src/fork_handlers.hpp). In the child, the caller holds
    // the lock lock_for_fork() returned; the waiters were threads of the
    // parent, so every ticket counts as completed.
    [[nodiscard]] std::unique_lock<std::mutex> lock_for_fork();
    void reset_in_fork_child() noexcept;

    // How often a waiting caller that delivers for another Core looks for a
    // cycle of flushes between Cores.
    static constexpr std::chrono::milliseconds cycle_check_interval{5};

private:
    [[nodiscard]] bool finished_locked_(uint64_t ticket) const noexcept {
        return completed_.load(std::memory_order_relaxed) >= ticket || closed_;
    }

    std::mutex mutex_;
    std::condition_variable changed_;     // waits on mutex_: a ticket completed, or closed
    std::atomic<uint64_t> requested_{0};  // written under mutex_; read without it by pending()
    std::atomic<uint64_t> completed_{0};  // written under mutex_; read without it by pending()
    bool closed_ = false;                 // guarded by mutex_
};

template <std::predicate F>
FlushRequests::Outcome FlushRequests::wait(uint64_t ticket, Clock::time_point deadline,
                                           bool check_cycle, F in_cycle) {
    std::unique_lock lock(mutex_);
    while (!finished_locked_(ticket)) {
        const Clock::time_point now = Clock::now();
        if (now >= deadline) {
            return Outcome::TimedOut;
        }
        if (check_cycle) {
            lock.unlock();
            const bool cycle = in_cycle();
            lock.lock();
            if (cycle && !finished_locked_(ticket)) {
                return Outcome::Cycle;
            }
            const Clock::time_point next_check = now + cycle_check_interval;
            static_cast<void>(
                changed_.wait_until(lock, next_check < deadline ? next_check : deadline));
        } else if (deadline == Clock::time_point::max()) {
            changed_.wait(lock);
        } else {
            static_cast<void>(changed_.wait_until(lock, deadline));
        }
    }
    return completed_.load(std::memory_order_relaxed) >= ticket ? Outcome::Done : Outcome::Closed;
}

}  // namespace lumen::detail

#endif
