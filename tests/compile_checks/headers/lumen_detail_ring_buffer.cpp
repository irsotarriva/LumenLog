// lumen/detail/ring_buffer.hpp alone declares the queue Core uses.
#include "lumen/detail/ring_buffer.hpp"

#include <type_traits>
#include <utility>

namespace lumen {

using Queue = RingBuffer<uint64_t, 4>;
static_assert(RingBufferElement<uint64_t>);
static_assert(std::is_same_v<decltype(std::declval<Queue&>().try_push(uint64_t{1})), bool>);
static_assert(
    std::is_same_v<decltype(std::declval<Queue&>().try_pop(std::declval<uint64_t&>())), bool>);
static_assert(std::is_same_v<decltype(std::declval<const Queue&>().head_position()), uint64_t>);
static_assert(std::is_same_v<decltype(std::declval<const Queue&>().tail_position()), uint64_t>);
static_assert(std::is_same_v<decltype(std::declval<const Queue&>().rejected_count()), uint64_t>);
static_assert(Queue::capacity() == 4);

}  // namespace lumen
