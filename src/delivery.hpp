#ifndef LUMEN_SRC_DELIVERY_HPP
#define LUMEN_SRC_DELIVERY_HPP

#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>

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
        explicit Session(Delivery& delivery) : lock_(delivery.mutex_), scope_(delivery) {}
        ~Session() = default;
        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;
        Session(Session&&) = delete;
        Session& operator=(Session&&) = delete;

    private:
        std::scoped_lock<std::mutex> lock_;
        DeliveryScope scope_;
    };

    // Whether the calling thread holds a Session of this Delivery anywhere on
    // its stack.
    [[nodiscard]] bool delivering_here() const noexcept;

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

    RecordQueues* queues_;    // non-owning: the Core's
    SinkRegistry* registry_;  // non-owning: the Core's
    FirstReports* reports_;   // non-owning: the Core's

    std::mutex mutex_;  // held by a Session
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
