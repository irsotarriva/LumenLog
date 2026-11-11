#ifndef LUMEN_SRC_WAKEUP_HPP
#define LUMEN_SRC_WAKEUP_HPP

#include <atomic>
#include <condition_variable>
#include <mutex>

#include "record_queues.hpp"

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

    // Dispatcher side. Returns once `queues` has a record or stop() was
    // called; false in the second case.
    //
    // Before blocking, a dispatcher that has just delivered records checks
    // the queues a few more times, a short interval apart. Records tend to
    // come in bursts; a dispatcher that blocked as soon as it caught up would
    // make the producer of the next record pay for waking it (a system call),
    // over and over within one burst. Sleeping between the checks, rather
    // than spinning, keeps it off the producers' cache lines meanwhile. An
    // idle dispatcher wakes up only these few times after its last record.
    [[nodiscard]] bool wait_for_work(const RecordQueues& queues);

    // Makes every current and later wait_for_work() return false.
    void stop() noexcept;

private:
    // True when one of the checks found a record; otherwise `sleeping_` is
    // set on return and the caller must block.
    [[nodiscard]] bool found_work_before_sleeping_(const RecordQueues& queues);
    void block_until_woken_();

    std::mutex mutex_;
    std::condition_variable woken_;      // waits on mutex_
    std::atomic<bool> sleeping_{false};  // see the protocol above
    std::atomic<bool> stopping_{false};  // set under mutex_
};

}  // namespace lumen::detail

#endif
