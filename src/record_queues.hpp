#ifndef LUMEN_SRC_RECORD_QUEUES_HPP
#define LUMEN_SRC_RECORD_QUEUES_HPP

#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

#include "lumen/core.hpp"
#include "lumen/detail/ring_buffer.hpp"
#include "lumen/limits.hpp"
#include "lumen/record.hpp"

namespace lumen::detail {

// A position in each of the three queues: head positions read at one moment
// (how far delivery must go), or tail positions (how far it went).
struct QueuePositions {
    uint64_t logs = 0;
    uint64_t metrics = 0;
    uint64_t progress = 0;
};

// Called by the consumer with each record it pops, whatever its type.
template <typename F>
concept RecordConsumer =
    std::invocable<F&, const LogRecord&> && std::invocable<F&, const MetricRecord&> &&
    std::invocable<F&, const ProgressRecord&>;

// The three bounded queues of a Core, one per record type, and per type the
// count of records refused because the Core had shut down. Producers push
// from any thread; one consumer at a time pops (the caller serialises the
// consumers, see Delivery).
class RecordQueues {
public:
    // Producer side. A refused record is counted by its queue (statistics()).
    // R is named by the caller, so `record` is an rvalue reference.
    template <RecordType R>
    [[nodiscard]] bool push(std::type_identity_t<R>&& record) noexcept {
        return lane_<R>().queue.try_push(std::move(record));
    }

    template <RecordType R>
    void count_after_shutdown() noexcept {
        lane_<R>().after_shutdown.fetch_add(1, std::memory_order_relaxed);
    }

    // Consumer side. Pops records until each queue's tail reaches its target
    // or the next record is not yet published, progress first, then
    // metrics, then logs, and hands each one to `consume`. Returns how many.
    template <RecordConsumer F>
    [[nodiscard]] uint64_t pop_up_to(const QueuePositions& targets, F consume) {
        return pop_lane_(progress_, targets.progress, consume) +
               pop_lane_(metrics_, targets.metrics, consume) +
               pop_lane_(logs_, targets.logs, consume);
    }

    [[nodiscard]] QueuePositions heads() const noexcept;
    // Whether every queue's tail has reached `targets`.
    [[nodiscard]] bool reached(const QueuePositions& targets) const noexcept;
    [[nodiscard]] bool has_pending() const noexcept;
    [[nodiscard]] uint64_t refused_when_full() const noexcept;

    template <RecordType R>
    [[nodiscard]] QueueStatistics statistics() const noexcept {
        const auto& lane = lane_<R>();
        return QueueStatistics{lane.queue.head_position(), lane.queue.rejected_count(),
                               lane.after_shutdown.load(std::memory_order_relaxed)};
    }

private:
    template <RecordType R, std::size_t N>
    struct Lane {
        RingBuffer<R, N> queue;
        std::atomic<uint64_t> after_shutdown{0};
    };

    template <RecordType R, std::size_t N, RecordConsumer F>
    [[nodiscard]] static uint64_t pop_lane_(Lane<R, N>& lane, uint64_t target, F& consume) {
        uint64_t popped = 0;
        R record;
        while (lane.queue.tail_position() < target && lane.queue.try_pop(record)) {
            consume(std::as_const(record));
            ++popped;
        }
        return popped;
    }

    template <RecordType R>
    [[nodiscard]] auto& lane_() noexcept {
        return lane_of_<R>(*this);
    }
    template <RecordType R>
    [[nodiscard]] const auto& lane_() const noexcept {
        return lane_of_<R>(*this);
    }
    // One body for the const and non-const lane_().
    template <RecordType R, typename Self>
        requires std::same_as<std::remove_const_t<Self>, RecordQueues>
    [[nodiscard]] static auto& lane_of_(Self& self) noexcept {
        if constexpr (std::same_as<R, LogRecord>) {
            return self.logs_;
        } else if constexpr (std::same_as<R, MetricRecord>) {
            return self.metrics_;
        } else {
            return self.progress_;
        }
    }

    Lane<LogRecord, log_queue_capacity> logs_;
    Lane<MetricRecord, metric_queue_capacity> metrics_;
    Lane<ProgressRecord, progress_queue_capacity> progress_;
};

}  // namespace lumen::detail

#endif
