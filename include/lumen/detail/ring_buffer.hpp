#ifndef LUMEN_DETAIL_RING_BUFFER_HPP
#define LUMEN_DETAIL_RING_BUFFER_HPP

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>

namespace lumen {

// An element must be movable without throwing: a producer that has claimed a
// slot must always publish it, or the consumer would wait on that slot forever.
template <typename T>
concept RingBufferElement = std::is_nothrow_move_constructible_v<T> &&
                            std::is_nothrow_move_assignable_v<T> &&
                            std::is_nothrow_destructible_v<T>;

// Bounded multi-producer, single-consumer FIFO queue (D. Vyukov's bounded
// queue, with the consumer side specialised to one thread).
//
// Threads. try_push() and push_blocking() may be called from any number of
// threads at once. try_pop() must be called by one thread at a time; successive
// consumers must be ordered by happens-before (Core uses a mutex). The
// observers (size, empty, head_position, tail_position, rejected_count) may be
// called from any thread and return a snapshot. The destructor must not run
// concurrently with any other call.
//
// Positions. Every accepted element gets a position p from the monotonic
// 64-bit counter head_ (it does not wrap in practice: 2^63 pushes at 10^9/s
// take 292 years) and lives in slot p % N. tail_ is the position of the next
// element the consumer will take. sequence_[p % N] encodes the slot's state:
//   2p      the slot is free for the producer that claims position p;
//   2p + 1  the element of position p is stored and may be popped.
// After popping position p the consumer stores 2(p + N), freeing the slot for
// the next lap. The factor two keeps "stored p" and "free for p + 1" distinct
// when N == 1.
//
// Push. A producer reads head_ = p and the slot's sequence s. If s == 2p it
// claims p by a CAS on head_, constructs the element in the slot and stores
// 2p + 1. If s < 2p the slot still holds position p - N, so N positions are
// claimed and not yet popped: the buffer is full. If s > 2p another producer
// has claimed p already, and the producer reloads head_.
//
// Memory order. Element data is handed over only through sequence_: the
// publishing store (release) pairs with the consumer's load (acquire), and the
// consumer's freeing store (release) pairs with the next producer's load
// (acquire), so a construction never overlaps the destruction of the previous
// element in that slot. head_ only arbitrates which producer owns a position
// and can be relaxed. tail_ is written by the consumer alone and is stored
// with release for observers.
//
// Ordering. Elements are popped in position order. try_pop() returns false
// while the oldest claimed element is still being written, even if later
// elements are ready.
//
// Storage. Slots are raw storage on the heap: an element exists only between
// its push and its pop, and the constructor does not write to it, so pages
// that are never used are never made resident. Slots are not padded to cache
// lines: for elements of several hundred bytes, two producers share at most
// the one line at a slot boundary, and padding would add up to 63 bytes per
// slot.
//
// Allocation failure in the constructor is not recoverable and terminates
// the program (std::bad_alloc escapes the noexcept constructor).
template <RingBufferElement T, std::size_t N>
    requires(std::has_single_bit(N))
class RingBuffer {
public:
    RingBuffer() noexcept
        : cells_(std::make_unique_for_overwrite<Cell[]>(N)),
          sequence_(std::make_unique<std::atomic<uint64_t>[]>(N)) {
        for (uint64_t pos = 0; pos < N; ++pos) {
            sequence_[pos].store(free_for(pos), std::memory_order_relaxed);
        }
    }

    ~RingBuffer() {
        const uint64_t head = head_.load(std::memory_order_relaxed);
        for (uint64_t pos = tail_.load(std::memory_order_relaxed); pos != head; ++pos) {
            std::destroy_at(&cells_[index_of(pos)].value);
        }
    }

    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;
    RingBuffer(RingBuffer&&) = delete;
    RingBuffer& operator=(RingBuffer&&) = delete;

    // Moves `item` into the buffer and returns true, or returns false if the
    // buffer is full. On false, `item` is untouched and rejected_count() grows
    // by one.
    [[nodiscard]] bool try_push(T&& item) noexcept {
        const std::optional<uint64_t> pos = try_claim_();
        if (!pos) {
            rejected_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        publish_(*pos, std::move(item));
        return true;
    }

    // Moves `item` into the buffer, yielding while the buffer is full. Never
    // rejects. Waits forever if no other thread is popping, so it must not be
    // called from the consumer's thread.
    void push_blocking(T&& item) noexcept {
        std::optional<uint64_t> pos = try_claim_();
        while (!pos) {
            std::this_thread::yield();
            pos = try_claim_();
        }
        publish_(*pos, std::move(item));
    }

    // Moves the oldest element into `out` and returns true, or returns false
    // (leaving `out` untouched) if the buffer is empty or the oldest element is
    // still being written. Single consumer: see the class comment.
    [[nodiscard]] bool try_pop(T& out) noexcept {
        const uint64_t pos = tail_.load(std::memory_order_relaxed);
        std::atomic<uint64_t>& sequence = sequence_[index_of(pos)];
        if (sequence.load(std::memory_order_acquire) != stored(pos)) {
            return false;
        }
        T& value = cells_[index_of(pos)].value;
        out = std::move(value);
        std::destroy_at(&value);
        sequence.store(free_for(pos + N), std::memory_order_release);
        tail_.store(pos + 1, std::memory_order_release);
        return true;
    }

    // Positions claimed and not yet popped, in [0, capacity()]. Exact only
    // while no other thread pushes or pops.
    [[nodiscard]] uint64_t size() const noexcept {
        // tail_ first: it never passes head_, so head - tail cannot underflow.
        const uint64_t tail = tail_.load(std::memory_order_acquire);
        const uint64_t head = head_.load(std::memory_order_acquire);
        return std::min<uint64_t>(head - tail, N);
    }

    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    // Number of positions claimed by producers so far (monotonic). A claimed
    // element may still be being written.
    [[nodiscard]] uint64_t head_position() const noexcept {
        return head_.load(std::memory_order_acquire);
    }

    // Number of elements popped so far (monotonic).
    [[nodiscard]] uint64_t tail_position() const noexcept {
        return tail_.load(std::memory_order_acquire);
    }

    // Number of try_push() calls that found the buffer full (monotonic).
    [[nodiscard]] uint64_t rejected_count() const noexcept {
        return rejected_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] static constexpr uint64_t capacity() noexcept { return N; }

private:
    // Storage for one element whose lifetime is managed by hand: the empty
    // constructor and destructor neither create nor destroy `value`.
    union Cell {
        Cell() noexcept {}
        ~Cell() {}
        Cell(const Cell&) = delete;
        Cell& operator=(const Cell&) = delete;
        Cell(Cell&&) = delete;
        Cell& operator=(Cell&&) = delete;
        T value;
    };

    // Producers and the consumer write different counters; keeping them on
    // separate cache lines avoids false sharing between the two sides.
    static constexpr std::size_t cache_line_size = 64;

    [[nodiscard]] static constexpr std::size_t index_of(uint64_t pos) noexcept {
        return static_cast<std::size_t>(pos & (N - 1));
    }
    [[nodiscard]] static constexpr uint64_t free_for(uint64_t pos) noexcept { return 2 * pos; }
    [[nodiscard]] static constexpr uint64_t stored(uint64_t pos) noexcept { return 2 * pos + 1; }

    // Claims the next position, or returns nullopt if the buffer is full.
    [[nodiscard]] std::optional<uint64_t> try_claim_() noexcept {
        uint64_t pos = head_.load(std::memory_order_relaxed);
        while (true) {
            const uint64_t sequence = sequence_[index_of(pos)].load(std::memory_order_acquire);
            if (sequence == free_for(pos)) {
                // On failure the CAS reloads `pos` and we look at the new slot.
                if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    return pos;
                }
            } else if (sequence < free_for(pos)) {
                return std::nullopt;
            } else {
                pos = head_.load(std::memory_order_relaxed);
            }
        }
    }

    // Stores `item` at a position this thread has claimed and hands it to the consumer.
    void publish_(uint64_t pos, T&& item) noexcept {
        std::construct_at(&cells_[index_of(pos)].value, std::move(item));
        sequence_[index_of(pos)].store(stored(pos), std::memory_order_release);
    }

    std::unique_ptr<Cell[]> cells_;
    std::unique_ptr<std::atomic<uint64_t>[]> sequence_;
    alignas(cache_line_size) std::atomic<uint64_t> head_{0};
    alignas(cache_line_size) std::atomic<uint64_t> tail_{0};
    alignas(cache_line_size) std::atomic<uint64_t> rejected_{0};
};

}  // namespace lumen

#endif
