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
// Protocol. It pairs two sequentially consistent operations on each side
// (no fences, which ThreadSanitizer does not support):
//   - the dispatcher sets `sleeping_` (seq_cst exchange), then checks the
//     queues once more, reading each head position (seq_cst load,
//     RingBuffer::head_position), and only then blocks;
//   - a producer claims its position (the seq_cst CAS on the queue's head in
//     RingBuffer::try_push), publishes its record, then reads `sleeping_`
//     (seq_cst load) and, if it is set, clears it with an exchange and
//     notifies when the exchange found it set.
// All four operations lie in one total order. If the producer's claim comes
// before the dispatcher's head load, the dispatcher sees the claim and does
// not block (it waits for the record to be published by retrying). If it
// comes after, the dispatcher's earlier store to `sleeping_` comes before
// the producer's later load, which therefore sees `true` (or a later
// clearing by another producer, which notifies). Either way no record waits
// for a sleeping dispatcher. The exchange makes one producer of several that
// saw `true` the one that notifies.
//
// Cost on every emit: one load of `sleeping_`. While the dispatcher is awake
// nobody writes that cache line, so producers on several cores share it
// without moving it between them. The exchange, the mutex and the notify (a
// system call) are only paid by a producer that finds the dispatcher asleep.
class Wakeup {
public:
    // Producer side, after RingBuffer::try_push accepted the record.
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
