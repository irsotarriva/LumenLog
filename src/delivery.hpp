#ifndef LUMEN_SRC_DELIVERY_HPP
#define LUMEN_SRC_DELIVERY_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>

#include "delivery_stack.hpp"
#include "first_reports.hpp"
#include "lumen/record.hpp"
#include "record_queues.hpp"
#include "sink_registry.hpp"

namespace lumen::detail {

// Hands a Core's queued records to its sinks. Whichever thread delivers (the
// dispatcher, flush(), a fatal record, shutdown) holds a Session: the queues
// have a single consumer, and sinks are never called concurrently.
//
// Every member except delivering_here() requires the calling thread to hold
// a Session of this Delivery, either its own or one further up its stack
// (inside a sink callback).
class Delivery {
public:
    Delivery(RecordQueues& queues, SinkRegistry& registry, FirstReports& reports) noexcept
        : queues_(&queues), registry_(&registry), reports_(&reports) {}

    // Holds the delivery lock and marks the calling thread as delivering for
    // this Delivery, for its lifetime.
    class Session {
    public:
        explicit Session(Delivery& delivery) : lock_(delivery.mutex_) { scope_.emplace(delivery); }
        // Tries for at most `limit`; owns() says whether it got the lock.
        Session(Delivery& delivery, std::chrono::milliseconds limit)
            : lock_(delivery.mutex_, std::defer_lock) {
            if (try_lock_within_(lock_, limit)) {
                scope_.emplace(delivery);
            }
        }
        ~Session() = default;
        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;
        Session(Session&&) = delete;
        Session& operator=(Session&&) = delete;

        [[nodiscard]] bool owns() const noexcept { return lock_.owns_lock(); }

    private:
        // Polls try_lock() rather than using a std::timed_mutex: GCC 13's
        // ThreadSanitizer does not see a lock taken by try_lock_for
        // (pthread_mutex_clocklock) and reports its unlock as an error. The
        // wait is bounded only on paths that give up (shutdown, a fatal
        // record, a flush looking for a cycle), so polling costs nothing
        // on the hot path.
        [[nodiscard]] static bool try_lock_within_(std::unique_lock<std::mutex>& lock,
                                                   std::chrono::milliseconds limit);

        std::unique_lock<std::mutex> lock_;
        std::optional<DeliveryScope> scope_;
    };

    // While it lives, marks every Delivery the calling thread holds a
    // Session of (none, for a thread outside any callback) as waiting for
    // `target`, so that in_cycle() can see a cycle of waits between Cores:
    // the thread delivering for X waits for Y, whose delivering thread waits
    // for X. The marks are stored and read sequentially consistently, so of
    // two threads that mark and then look, at least one sees the other.
    class Waiting {
    public:
        explicit Waiting(const Delivery& target) noexcept;
        ~Waiting();
        Waiting(const Waiting&) = delete;
        Waiting& operator=(const Waiting&) = delete;
        Waiting(Waiting&&) = delete;
        Waiting& operator=(Waiting&&) = delete;

        // Whether the calling thread delivers for any Delivery (so that it
        // can be part of a cycle at all).
        [[nodiscard]] bool can_be_in_cycle() const noexcept { return marked_; }
        // Whether following the marks from `target` leads back to a Delivery
        // the calling thread holds: waiting would then never end.
        [[nodiscard]] bool in_cycle() const noexcept;

    private:
        const Delivery* target_;  // non-owning
        bool marked_ = false;
    };

    // Whether the calling thread holds a Session of this Delivery anywhere on
    // its stack.
    [[nodiscard]] bool delivering_here() const noexcept;

    // Records delivered and sink flushes made so far; it moves while
    // delivery makes progress. Any thread.
    [[nodiscard]] uint64_t progress() const noexcept {
        return progress_.load(std::memory_order_relaxed);
    }

    // Fork handler (src/fork_handlers.hpp), for a child in which the calling
    // thread holds no Session of this Delivery: whoever held one was a
    // thread of the parent, so the lock is re-initialised. The snapshot is
    // kept; SinkRegistry replaces it under its own lock, which the fork
    // handlers hold, so it is whole.
    void reset_in_fork_child() noexcept;

    // One pass up to `targets`; stops early at a record not yet published.
    // Returns how many records were delivered.
    [[nodiscard]] uint64_t deliver_up_to(const QueuePositions& targets);
    // Up to `targets`, waiting for producers that claimed a slot before the
    // targets were read to publish it.
    void deliver_all_up_to(const QueuePositions& targets);
    void deliver_everything_queued();

    // A record that did not come through the queues (a fatal record).
    void dispatch(const LogRecord& record);

    void flush_sinks();

    // Finishes the record this thread was handing out when a callback raised
    // a fatal record or shut the Core down.
    void finish_in_flight();

    // At shutdown: lets the registry destroy the sinks.
    void release_snapshot() noexcept;

private:
    void dispatch_(InFlight::Record record);
    // Hands the frame's record to the sinks it has not been offered to yet.
    void continue_(InFlight& in_flight);
    [[nodiscard]] const Registry& snapshot_();
    void count_progress_() noexcept {
        progress_.store(progress_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }

    RecordQueues* queues_;    // non-owning: the Core's
    SinkRegistry* registry_;  // non-owning: the Core's
    FirstReports* reports_;   // non-owning: the Core's

    std::mutex mutex_;  // held by a Session
    // The Delivery whose progress the thread holding a Session of this one
    // waits for (see Waiting), or null.
    mutable std::atomic<const Delivery*> waits_for_{nullptr};
    std::atomic<uint64_t> progress_{0};  // written by the Session holder
    // The registry as the delivering thread sees it: a snapshot refreshed
    // when the registry's version moves, so delivering a record takes no
    // lock. A callback that adds or removes a sink replaces the registry,
    // not this snapshot. A delivery nested inside a callback (a fatal record)
    // keeps the snapshot, so the loops further up the stack never see theirs
    // freed.
    std::shared_ptr<const Registry> snapshot_sinks_;                    // guarded by mutex_
    uint64_t snapshot_version_ = std::numeric_limits<uint64_t>::max();  // guarded by mutex_
    uint32_t snapshot_loops_ = 0;  // loops over snapshot_sinks_ running; guarded by mutex_
};

}  // namespace lumen::detail

#endif
