#ifndef LUMEN_SRC_WAKEUP_HPP
#define LUMEN_SRC_WAKEUP_HPP

#include <atomic>
#include <concepts>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace lumen::detail {

// How producers wake a sleeping dispatch thread, and how that thread waits
// for records without missing one.
//
// Protocol. Both sides update `sleeping_` with a read-modify-write
// (acq_rel), so the two updates are ordered in its modification order and
// the later one sees the earlier:
//   - the dispatcher sets it with an exchange, then checks the queues once
//     more, and only then blocks;
//   - a producer, after publishing its record, clears it with an exchange
//     and notifies if it was set.
// If the producer's exchange comes second, it sees `true` and wakes the
// dispatcher. If it comes first, the dispatcher's exchange reads from it
// (acquire), so the record published before it is visible to the second
// check. Either way no record waits for a sleeping dispatcher. (A seq_cst
// fence on each side would do the same, but ThreadSanitizer does not support
// fences.)
//
// Cost on every emit: one atomic exchange on a shared cache line (a locked
// instruction, a few tens of cycles; producers on several cores also move
// that line between them). The mutex and the notify (a system call) are only
// paid by a producer that finds the dispatcher asleep.
class Wakeup {
public:
    // Producer side, after the record is published.
    void wake_if_sleeping() noexcept;

    // Dispatcher side. Returns once `has_work()` (a record in the queues, or
    // a flush requested) or stop() was called; false in the second case. A
    // producer of work makes `has_work()` true before wake_if_sleeping().
    //
    // Before blocking, a dispatcher that has just delivered records checks
    // the queues a few more times, a short interval apart. Records tend to
    // come in bursts; a dispatcher that blocked as soon as it caught up would
    // make the producer of the next record pay for waking it (a system call),
    // over and over within one burst. Sleeping between the checks, rather
    // than spinning, keeps it off the producers' cache lines meanwhile. An
    // idle dispatcher wakes up only these few times after its last record.
    template <std::predicate F>
    [[nodiscard]] bool wait_for_work(F has_work);

    // Makes every current and later wait_for_work() return false.
    void stop() noexcept;

    // Fork handlers (src/fork_handlers.hpp). In the child, the caller holds
    // the lock lock_for_fork() returned; the dispatch thread that may have
    // been asleep is not there.
    [[nodiscard]] std::unique_lock<std::mutex> lock_for_fork();
    void reset_in_fork_child() noexcept;

private:
    // True when one of the checks found work; otherwise `sleeping_` is set
    // on return and the caller must block.
    template <std::predicate F>
    [[nodiscard]] bool found_work_before_sleeping_(F& has_work);
    void block_until_woken_();
    // Sleeps for idle_check_interval_.
    static void pause_between_checks_();

    static constexpr uint32_t idle_check_count = 5;

    std::mutex mutex_;
    std::condition_variable woken_;      // waits on mutex_
    std::atomic<bool> sleeping_{false};  // see the protocol above
    std::atomic<bool> stopping_{false};  // set under mutex_
};

template <std::predicate F>
bool Wakeup::wait_for_work(F has_work) {
    if (stopping_.load(std::memory_order_acquire)) {
        return false;
    }
    if (found_work_before_sleeping_(has_work)) {
        return true;
    }
    block_until_woken_();
    return !stopping_.load(std::memory_order_relaxed);
}

template <std::predicate F>
bool Wakeup::found_work_before_sleeping_(F& has_work) {
    for (uint32_t check = 0; check < idle_check_count && !has_work(); ++check) {
        pause_between_checks_();
    }
    if (has_work()) {
        return true;
    }
    // The previous value is false: only this thread sets the flag.
    static_cast<void>(sleeping_.exchange(true, std::memory_order_acq_rel));
    if (has_work()) {
        sleeping_.store(false, std::memory_order_relaxed);
        return true;
    }
    return false;
}

}  // namespace lumen::detail

#endif
