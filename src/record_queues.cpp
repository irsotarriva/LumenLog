#include "record_queues.hpp"

namespace lumen::detail {

QueuePositions RecordQueues::heads() const noexcept {
    return QueuePositions{logs_.queue.head_position(), metrics_.queue.head_position(),
                          progress_.queue.head_position()};
}

bool RecordQueues::reached(const QueuePositions& targets) const noexcept {
    return logs_.queue.tail_position() >= targets.logs &&
           metrics_.queue.tail_position() >= targets.metrics &&
           progress_.queue.tail_position() >= targets.progress;
}

bool RecordQueues::has_pending() const noexcept {
    return logs_.queue.head_position() != logs_.queue.tail_position() ||
           metrics_.queue.head_position() != metrics_.queue.tail_position() ||
           progress_.queue.head_position() != progress_.queue.tail_position();
}

uint64_t RecordQueues::queued() const noexcept {
    return logs_.queue.size() + metrics_.queue.size() + progress_.queue.size();
}

void RecordQueues::discard_in_fork_child() noexcept {
    logs_.queue.discard_in_fork_child();
    metrics_.queue.discard_in_fork_child();
    progress_.queue.discard_in_fork_child();
}

uint64_t RecordQueues::refused_when_full() const noexcept {
    return logs_.queue.rejected_count() + metrics_.queue.rejected_count() +
           progress_.queue.rejected_count();
}

}  // namespace lumen::detail
