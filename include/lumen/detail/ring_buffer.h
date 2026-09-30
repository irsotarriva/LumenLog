#ifndef LUMEN_DETAIL_RING_BUFFER_H
#define LUMEN_DETAIL_RING_BUFFER_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>

#include "lumen/detail/overflow_policy.h"

namespace lumen {

template <typename T, size_t N>
class RingBuffer {
    static_assert(N > 0 && (N & (N - 1)) == 0, "RingBuffer capacity must be a power of two");

    static constexpr size_t __capacity = N;
    static constexpr size_t __mask = N - 1;

    struct Slot {
        alignas(64) std::atomic<uint64_t> sequence;
        T data;
    };

public:
    RingBuffer() {
        for (size_t i = 0; i < __capacity; ++i) {
            __slots[i].sequence.store(i, std::memory_order_relaxed);
        }
        // Initialize elements that are not trivially default-constructible
        if constexpr (!std::is_trivially_default_constructible_v<T>) {
            for (size_t i = 0; i < __capacity; ++i) {
                new (&__slots[i].data) T();
            }
        }
    }

    ~RingBuffer() {
        // Destroy any unconsumed elements
        if constexpr (!std::is_trivially_destructible_v<T>) {
            uint64_t t = __tail.load(std::memory_order_relaxed);
            uint64_t h = __head.load(std::memory_order_relaxed);
            while (t < h) {
                size_t idx = t & __mask;
                uint64_t seq = __slots[idx].sequence.load(std::memory_order_relaxed);
                if (seq == t + 1) {
                    __slots[idx].data.~T();
                }
                ++t;
            }
        }
    }

    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;
    RingBuffer(RingBuffer&&) = delete;
    RingBuffer& operator=(RingBuffer&&) = delete;

    template <OverflowPolicy Policy = OverflowPolicy::DROP_NEWEST, typename U = T>
    bool push(U&& item) {
        while (true) {
            uint64_t pos = __head.load(std::memory_order_relaxed);
            size_t idx = pos & __mask;
            uint64_t seq = __slots[idx].sequence.load(std::memory_order_acquire);
            int64_t diff = static_cast<int64_t>(seq) - static_cast<int64_t>(pos);

            if (diff == 0) {
                if (__head.compare_exchange_weak(pos, pos + 1, std::memory_order_acq_rel)) {
                    __slots[idx].data = std::forward<U>(item);
                    __slots[idx].sequence.store(pos + 1, std::memory_order_release);
                    return true;
                }
                continue;
            }

            if (diff > 0) {
                // Another producer claimed `pos` after we read __head; reload
                // and retry. Only diff < 0 means the buffer is full.
                continue;
            }

            if constexpr (Policy == OverflowPolicy::DROP_NEWEST) {
                return false;
            } else if constexpr (Policy == OverflowPolicy::DROP_OLDEST) {
                if (diff < 0) {
                    uint64_t t = __tail.load(std::memory_order_relaxed);
                    if (pos - t >= __capacity) {
                        if (__tail.compare_exchange_weak(t, t + 1, std::memory_order_acq_rel)) {
                            size_t tail_idx = t & __mask;
                            __slots[tail_idx].sequence.store(t + __capacity, std::memory_order_release);
                        }
                    }
                }
                continue;
            } else {
                while (__slots[idx].sequence.load(std::memory_order_acquire) != pos) {
                    std::this_thread::yield();
                }
                continue;
            }
        }
    }

    bool pop(T& item) {
        while (true) {
            uint64_t t = __tail.load(std::memory_order_relaxed);
            size_t idx = t & __mask;
            uint64_t seq = __slots[idx].sequence.load(std::memory_order_acquire);
            int64_t diff = static_cast<int64_t>(seq) - static_cast<int64_t>(t + 1);

            if (diff == 0) {
                item = std::move(__slots[idx].data);
                __slots[idx].sequence.store(t + __capacity, std::memory_order_release);
                __tail.store(t + 1, std::memory_order_release);
                return true;
            }

            if (diff > 0) {
                __tail.store(t + 1, std::memory_order_release);
                continue;
            }

            return false;
        }
    }

    uint64_t size() const {
        uint64_t h = __head.load(std::memory_order_acquire);
        uint64_t t = __tail.load(std::memory_order_acquire);
        return h - t;
    }

    bool empty() const {
        return size() == 0;
    }

    // Number of slots claimed by producers so far (monotonic).
    [[nodiscard]] uint64_t head_position() const { return __head.load(std::memory_order_acquire); }
    // Number of slots released by the consumer so far (monotonic).
    [[nodiscard]] uint64_t tail_position() const { return __tail.load(std::memory_order_acquire); }

    static constexpr uint64_t capacity() { return __capacity; }

private:
    std::array<Slot, __capacity> __slots;
    alignas(64) std::atomic<uint64_t> __head{0};
    alignas(64) std::atomic<uint64_t> __tail{0};
};

}  // namespace lumen

#endif
