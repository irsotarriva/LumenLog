#ifndef LUMEN_DETAIL_ARENA_H
#define LUMEN_DETAIL_ARENA_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>

namespace lumen {

class Arena {
public:
    static constexpr size_t DEFAULT_CAPACITY = 4096;

    Arena() = default;

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    Arena(Arena&&) = delete;
    Arena& operator=(Arena&&) = delete;

    char* allocate(size_t n) {
        if (__using_spill) {
            return __spill_alloc(n);
        }

        size_t aligned = (__offset + alignof(std::max_align_t) - 1) & ~(alignof(std::max_align_t) - 1);
        if (aligned + n <= DEFAULT_CAPACITY) {
            char* ptr = __buffer.data() + aligned;
            __offset = aligned + n;
            return ptr;
        }

        __spill_from_buffer();
        __using_spill = true;
        __warned = true;
        std::fprintf(stderr, "lumen: arena overflow (%zu bytes, capacity=%zu)\n", n, DEFAULT_CAPACITY);
        return __spill_alloc(n);
    }

    void reset() {
        __offset = 0;
        __spill.clear();
        __using_spill = false;
        __warned = false;
    }

    size_t used() const {
        if (__using_spill) return __spill.size();
        return __offset;
    }

    size_t remaining() const {
        if (__using_spill) return 0;
        size_t aligned = (__offset + alignof(std::max_align_t) - 1) & ~(alignof(std::max_align_t) - 1);
        if (aligned >= DEFAULT_CAPACITY) return 0;
        return DEFAULT_CAPACITY - aligned;
    }

private:
    void __spill_from_buffer() {
        __spill.assign(__buffer.data(), __offset);
    }

    char* __spill_alloc(size_t n) {
        if (!__warned) {
            std::fprintf(stderr, "lumen: arena overflow (%zu bytes, capacity=%zu)\n", n, DEFAULT_CAPACITY);
            __warned = true;
        }
        size_t old_size = __spill.size();
        __spill.resize(old_size + n);
        return __spill.data() + old_size;
    }

    alignas(alignof(std::max_align_t)) std::array<char, DEFAULT_CAPACITY> __buffer{};
    size_t __offset = 0;
    std::string __spill;
    bool __using_spill = false;
    bool __warned = false;
};

inline thread_local Arena this_thread_arena;

}  // namespace lumen

#endif
