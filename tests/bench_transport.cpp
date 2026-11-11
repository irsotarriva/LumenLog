// Throughput of the ring buffer that carries records from producers to the
// dispatch thread. Measures the transport alone: no record building, no
// dispatch, no sinks. Each run starts P producer threads that offer K elements
// each while one consumer thread pops, and reports separately how many
// elements were accepted, rejected (buffer full) and delivered.
//
// This is a measurement, not a test, and is not registered with ctest. Build
// and run it in Release:
//   cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release -DLUMEN_ENABLE_DASHBOARD=OFF
//   cmake --build build/release --target lumen_bench_transport
//   build/release/tests/lumen_bench_transport
// The exit status is non-zero only if delivered != accepted, which would be a
// transport defect.

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iostream>
#include <string_view>
#include <thread>
#include <vector>

#include "lumen/detail/ring_buffer.hpp"
#include "lumen/limits.hpp"
#include "lumen/record.hpp"

namespace {

// As large as a LogRecord, so that copying it costs what moving a record costs.
struct Payload {
    uint64_t value = 0;
    std::array<std::byte, sizeof(lumen::LogRecord) - sizeof(uint64_t)> padding{};
};

using Buffer = lumen::RingBuffer<Payload, lumen::log_queue_capacity>;
using Clock = std::chrono::steady_clock;

struct RunResult {
    uint64_t offered = 0;
    uint64_t accepted = 0;
    uint64_t rejected = 0;
    uint64_t delivered = 0;
    double producer_seconds = 0;  // until the last producer returned
    double total_seconds = 0;     // until the consumer had popped everything
};

// Offers `count` elements with try_push, which drops when the buffer is
// full, as the logging front end does.
[[nodiscard]] uint64_t produce(Buffer& buffer, uint64_t count) {
    uint64_t accepted = 0;
    for (uint64_t i = 0; i < count; ++i) {
        if (buffer.try_push(Payload{.value = i})) {
            ++accepted;
        }
    }
    return accepted;
}

// Pops until `producers` have finished and the buffer is empty.
[[nodiscard]] uint64_t consume(Buffer& buffer, const std::atomic<uint32_t>& finished,
                               uint32_t producers) {
    uint64_t delivered = 0;
    Payload out;
    while (true) {
        const bool done = finished.load(std::memory_order_acquire) == producers;
        if (buffer.try_pop(out)) {
            ++delivered;
        } else if (done) {
            return delivered;
        } else {
            std::this_thread::yield();
        }
    }
}

[[nodiscard]] RunResult run(uint32_t producers, uint64_t per_producer) {
    // A new buffer per run, so that every run first-touches its slots.
    Buffer buffer;
    std::atomic<bool> start{false};
    std::atomic<uint32_t> finished{0};
    std::atomic<uint64_t> accepted{0};
    Clock::time_point producers_end;  // written by the last producer, read after join

    std::vector<std::thread> threads;
    for (uint32_t p = 0; p < producers; ++p) {
        threads.emplace_back([&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            accepted.fetch_add(produce(buffer, per_producer), std::memory_order_relaxed);
            const Clock::time_point now = Clock::now();
            if (finished.fetch_add(1, std::memory_order_release) + 1 == producers) {
                producers_end = now;
            }
        });
    }
    uint64_t delivered = 0;
    std::thread consumer([&] { delivered = consume(buffer, finished, producers); });

    const Clock::time_point begin = Clock::now();
    start.store(true, std::memory_order_release);
    for (std::thread& t : threads) {
        t.join();
    }
    consumer.join();
    const Clock::time_point end = Clock::now();

    return RunResult{
        .offered = producers * per_producer,
        .accepted = accepted.load(),
        .rejected = buffer.rejected_count(),
        .delivered = delivered,
        .producer_seconds = std::chrono::duration<double>(producers_end - begin).count(),
        .total_seconds = std::chrono::duration<double>(end - begin).count(),
    };
}

}  // namespace

int main() {
#ifndef NDEBUG
    std::cout << "warning: assertions are enabled; numbers from a Debug build are not meaningful\n";
#endif
    constexpr uint64_t total_per_run = 2'000'000;
    std::cout << std::format("element size {} B, capacity {}, {} elements offered per run\n",
                             sizeof(Payload), Buffer::capacity(), total_per_run);
    std::cout << std::format("{:>4}{:>11}{:>11}{:>11}{:>16}{:>16}\n", "P", "accepted", "rejected",
                             "delivered", "accepted/s", "delivered/s");

    bool consistent = true;
    for (const uint32_t producers : {1U, 2U, 4U, 8U}) {
        const RunResult r = run(producers, total_per_run / producers);
        consistent =
            consistent && r.delivered == r.accepted && r.accepted + r.rejected == r.offered;
        std::cout << std::format("{:>4}{:>11}{:>11}{:>11}{:>16.0f}{:>16.0f}\n", producers,
                                 r.accepted, r.rejected, r.delivered,
                                 static_cast<double>(r.accepted) / r.producer_seconds,
                                 static_cast<double>(r.delivered) / r.total_seconds);
    }
    if (!consistent) {
        std::cout << "error: delivered != accepted or accepted + rejected != offered\n";
        return 1;
    }
    return 0;
}
