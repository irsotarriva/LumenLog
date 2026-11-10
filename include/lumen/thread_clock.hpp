#ifndef LUMEN_THREAD_CLOCK_HPP
#define LUMEN_THREAD_CLOCK_HPP

#include <atomic>
#include <chrono>
#include <cstdint>

// The two stamps Core puts on every record: when, and on which thread.

namespace lumen {

// Wall-clock time in nanoseconds since the Unix epoch (std::chrono::system_clock).
[[nodiscard]] inline uint64_t now_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count());
}

// A small number naming the calling thread: 1 for the first thread that asks,
// 2 for the next, and so on. Two threads never share a number (until 2^32
// threads have asked), and a thread keeps its number for its whole life.
// Numbers are not reused after a thread exits.
[[nodiscard]] inline uint32_t this_thread_id() {
    // The one counter for the whole program (an inline function's static is
    // shared by every translation unit).
    static std::atomic<uint32_t> next_id{1};
    thread_local const uint32_t id = next_id.fetch_add(1, std::memory_order_relaxed);
    return id;
}

}  // namespace lumen

#endif
