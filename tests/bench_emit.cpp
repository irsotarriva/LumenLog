// Cost of one producer statement: the time a thread spends in a log
// statement, lumen::metric or a progress update, from the start of the
// statement to its return, and the heap allocations it makes. The dispatch
// thread runs at the same time and delivers to a NullSink, as it would in a
// program; its own work is not counted.
//
// Method. Statements run in batches of `batch_size`; each batch is timed
// with steady_clock and gives one sample, the mean time of a statement in
// that batch. Between rounds of `batches_per_round` batches the Core is
// flushed, outside the timing, so that no queue fills up and no record is
// dropped (a dropped record costs less than a queued one). Each case
// reports the minimum, median and 99th percentile of its samples, the
// allocations per statement on the producing thread, and the records
// dropped (which must be 0 for the numbers to mean anything). Where the
// platform allows it, each producer thread is pinned to its own CPU.
//
// This is a measurement, not a test, and is not registered with ctest. Build
// and run it in Release:
//   cmake --preset release -DLUMEN_ENABLE_DASHBOARD=OFF
//   cmake --build --preset release --target lumen_bench_emit
//   build/release/tests/lumen_bench_emit

#include <algorithm>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "allocation_counter.hpp"
#include "lumen/lumen.hpp"

#ifdef PLATFORM_LINUX
#include <pthread.h>
#include <sched.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

inline constexpr uint64_t batch_size = 128;
// 16 batches of 128: 2048 records per round, half of the smallest queue.
inline constexpr uint64_t batches_per_round = 16;
inline constexpr uint64_t rounds = 200;

// One statement under measurement; the argument changes with each call so
// that the formatted cases format a different number every time.
using Statement = std::function<void(uint64_t)>;

void pin_to_cpu(uint32_t cpu) {
#ifdef PLATFORM_LINUX
    const uint32_t cpus = std::max(1U, std::thread::hardware_concurrency());
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu % cpus, &set);
    // Unpinned numbers are still numbers; the report says pinning was asked for.
    static_cast<void>(pthread_setaffinity_np(pthread_self(), sizeof(set), &set));
#else
    static_cast<void>(cpu);
#endif
}

struct Samples {
    std::vector<double> ns_per_statement;  // one per batch
    uint64_t statements = 0;
    uint64_t allocations = 0;
};

// Runs one round of batches on the calling thread and appends its samples.
void run_round(const Statement& statement, Samples& out) {
    for (uint64_t b = 0; b < batches_per_round; ++b) {
        const uint64_t allocations_before = lumen::test::thread_allocations;
        const Clock::time_point begin = Clock::now();
        for (uint64_t i = 0; i < batch_size; ++i) {
            statement((b * batch_size) + i);
        }
        const Clock::time_point end = Clock::now();
        out.allocations += lumen::test::thread_allocations - allocations_before;
        out.statements += batch_size;
        out.ns_per_statement.push_back(
            std::chrono::duration<double, std::nano>(end - begin).count() /
            static_cast<double>(batch_size));
    }
}

[[nodiscard]] uint64_t dropped_records() {
    const lumen::CoreStatistics s = lumen::core().statistics();
    return s.logs.dropped_queue_full + s.metrics.dropped_queue_full + s.progress.dropped_queue_full;
}

void report(std::string_view name, uint32_t producers, Samples& samples, uint64_t dropped) {
    std::vector<double>& v = samples.ns_per_statement;
    std::ranges::sort(v);
    const auto at = [&v](double quantile) {
        return v[static_cast<std::size_t>(quantile * static_cast<double>(v.size() - 1))];
    };
    std::cout << std::format(
        "{:<34}{:>3}{:>9.0f}{:>9.0f}{:>9.0f}{:>9.2f}{:>9}\n", name, producers, v.front(), at(0.5),
        at(0.99),
        static_cast<double>(samples.allocations) / static_cast<double>(samples.statements),
        dropped);
}

// One producer: the calling thread, pinned to CPU 1 (the dispatch thread
// is left to the scheduler).
void measure_single(std::string_view name, const Statement& statement) {
    pin_to_cpu(1);
    Samples warm_up;
    run_round(statement, warm_up);
    lumen::core().flush();
    const uint64_t dropped_before = dropped_records();
    Samples samples;
    for (uint64_t r = 0; r < rounds; ++r) {
        run_round(statement, samples);
        lumen::core().flush();
    }
    report(name, 1, samples, dropped_records() - dropped_before);
}

// `producers` threads run the same rounds in step; the main thread flushes
// between rounds while they wait.
void measure_parallel(std::string_view name, uint32_t producers, const Statement& statement) {
    std::barrier sync(static_cast<std::ptrdiff_t>(producers) + 1);
    std::vector<Samples> per_thread(producers);
    const uint64_t dropped_before = dropped_records();
    std::vector<std::thread> threads;
    for (uint32_t p = 0; p < producers; ++p) {
        threads.emplace_back([&, p] {
            pin_to_cpu(p);
            Samples warm_up;
            run_round(statement, warm_up);
            for (uint64_t r = 0; r < rounds; ++r) {
                sync.arrive_and_wait();  // round starts
                run_round(statement, per_thread[p]);
                sync.arrive_and_wait();  // round done
            }
        });
    }
    for (uint64_t r = 0; r < rounds; ++r) {
        sync.arrive_and_wait();
        sync.arrive_and_wait();
        lumen::core().flush();
    }
    for (std::thread& t : threads) {
        t.join();
    }
    Samples all;
    for (Samples& s : per_thread) {
        all.statements += s.statements;
        all.allocations += s.allocations;
        all.ns_per_statement.insert(all.ns_per_statement.end(), s.ns_per_statement.begin(),
                                    s.ns_per_statement.end());
    }
    report(name, producers, all, dropped_records() - dropped_before);
}

// Returns false if a table refused a tag (the case would then measure less
// context than its name says).
[[nodiscard]] bool set_ambient_tags(uint32_t process_tags, uint32_t thread_tags) {
    bool all_set = true;
    for (uint32_t i = 0; i < process_tags; ++i) {
        all_set =
            lumen::set_process_tag(std::format("process_{}", i), "value").has_value() && all_set;
    }
    for (uint32_t i = 0; i < thread_tags; ++i) {
        all_set =
            lumen::set_thread_tag(std::format("thread_{}", i), "value").has_value() && all_set;
    }
    return all_set;
}

}  // namespace

int main() {
#ifndef NDEBUG
    std::cout << "warning: assertions are enabled; numbers from a Debug build are not meaningful\n";
#endif
    if (!lumen::core().add_sink(std::make_unique<lumen::NullSink>())) {
        std::cout << "error: could not add the sink\n";
        return 1;
    }
    std::cout << std::format("{} statements per case and producer, batches of {}\n",
                             rounds * batches_per_round * batch_size, batch_size);
    std::cout << std::format("{:<34}{:>3}{:>9}{:>9}{:>9}{:>9}{:>9}\n", "case (ns per statement)",
                             "P", "min", "median", "p99", "allocs", "dropped");

    const Statement plain = [](uint64_t) { LUMEN_LOG_INFO("plain message"); };
    const Statement formatted = [](uint64_t i) { LUMEN_LOG_INFO("value {}", i); };
    measure_single("plain message", plain);
    measure_single("plain message + 2 tags", [](uint64_t i) {
        LUMEN_LOG_INFO("plain message").tag("run", "r1").tag("event", i);
    });
    measure_single("formatted message", formatted);
    measure_single("metric", [](uint64_t i) { lumen::metric("loss", static_cast<double>(i)); });
    lumen::ProgressHandle bar = lumen::progress("items", uint64_t{1} << 40U);
    measure_single("progress tick", [&bar](uint64_t) { bar.tick(); });
    measure_parallel("plain message", 4, plain);
    measure_parallel("formatted message", 4, formatted);
    // Ambient tags last: thread tags stay on this thread.
    if (!set_ambient_tags(2, 2)) {
        std::cout << "error: an ambient tag was refused\n";
        return 1;
    }
    measure_single("formatted, 4 ambient tags", formatted);
    if (!set_ambient_tags(8, 8)) {
        std::cout << "error: an ambient tag was refused\n";
        return 1;
    }
    measure_single("formatted, 16 ambient tags", formatted);
    return 0;
}
