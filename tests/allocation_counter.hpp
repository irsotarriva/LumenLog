#ifndef LUMEN_TESTS_ALLOCATION_COUNTER_HPP
#define LUMEN_TESTS_ALLOCATION_COUNTER_HPP

// Replaces the global operator new and delete with versions that count, per
// thread, the allocations of the calling thread. Include it in exactly one
// source file of an executable: it defines the replacement functions, which
// then serve the whole program, the library included.

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

namespace lumen::test {

// Allocations made by the calling thread so far. The single writer is the
// replaced operator new below, running on that thread.
inline thread_local uint64_t thread_allocations = 0;  // NOLINT(*-avoid-non-const-global-variables)

// The allocations `statement` makes on the calling thread.
template <std::invocable F>
[[nodiscard]] uint64_t allocations_during(F statement) {
    const uint64_t before = thread_allocations;
    statement();
    return thread_allocations - before;
}

}  // namespace lumen::test

// A failed allocation ends the program: there is nothing left to count then.
void* operator new(std::size_t size) {
    ++lumen::test::thread_allocations;
    void* const block = std::malloc(size == 0 ? 1 : size);  // NOLINT(*-no-malloc, *-owning-memory)
    if (block == nullptr) {
        std::abort();
    }
    return block;
}

void* operator new[](std::size_t size) {
    return operator new(size);
}

// The nothrow forms too, so that every block the replaced delete frees came
// from malloc (a sanitizer's own operator new would not match free()).
void* operator new(std::size_t size, const std::nothrow_t& /*tag*/) noexcept {
    return operator new(size);
}

void* operator new[](std::size_t size, const std::nothrow_t& /*tag*/) noexcept {
    return operator new(size);
}

// GCC sees through the replaced operator new after inlining and takes the
// free() below for a mismatched deallocation; here it is the matching one.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

void operator delete(void* block) noexcept {
    std::free(block);  // NOLINT(*-no-malloc, *-owning-memory)
}

void operator delete(void* block, std::size_t /*size*/) noexcept {
    std::free(block);  // NOLINT(*-no-malloc, *-owning-memory)
}

void operator delete[](void* block) noexcept {
    std::free(block);  // NOLINT(*-no-malloc, *-owning-memory)
}

void operator delete[](void* block, std::size_t /*size*/) noexcept {
    std::free(block);  // NOLINT(*-no-malloc, *-owning-memory)
}

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#endif
