#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "lumen/core.h"
#include "lumen/predicate.h"
#include "lumen/sink.h"

namespace lumen {
namespace {

struct NullSink : public Sink {
    void on_log(const LogRecord&) override {}
    void on_metric(const MetricRecord&) override {}
    void on_progress(const ProgressRecord&) override {}
    void flush() override {}
};

class BenchTransport : public ::testing::Test {
protected:
    SinkId sink_id_ = 0;

    void SetUp() override {
        sink_id_ = core().add_sink(std::make_unique<NullSink>(), always());
    }

    void TearDown() override {
        if (sink_id_ != 0) {
            (void)core().remove_sink(sink_id_);
            sink_id_ = 0;
        }
    }
};

int64_t run_producers(int num_producers, int64_t records_per_producer) {
    std::atomic<bool> start{false};
    std::atomic<int64_t> total{0};

    std::vector<std::thread> producers;
    for (int p = 0; p < num_producers; ++p) {
        producers.emplace_back([&, records_per_producer]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (int64_t i = 0; i < records_per_producer; ++i) {
                RecordBuilder builder(core().log_buffer(), LogLevel::INFO,
                                      "benchmark message record");
                builder.tag("thread", static_cast<int64_t>(
                    std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xFFFF));
            }

            total.fetch_add(records_per_producer, std::memory_order_relaxed);
        });
    }

    start.store(true, std::memory_order_release);
    auto t0 = std::chrono::high_resolution_clock::now();

    for (auto& t : producers) {
        if (t.joinable()) {
            t.join();
        }
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    int64_t total_records = num_producers * records_per_producer;
    int64_t records_per_sec = elapsed_ns > 0
        ? (total_records * 1'000'000'000LL) / elapsed_ns
        : 0;

    std::printf(
        "  producers=%d  records=%lld  elapsed=%lld ms  throughput=%lld rec/s\n",
        num_producers,
        static_cast<long long>(total_records),
        static_cast<long long>(elapsed_ms),
        static_cast<long long>(records_per_sec));

    return records_per_sec;
}

TEST_F(BenchTransport, RecordsPerSec1Producer) {
    int64_t rate = run_producers(1, 100'000);
    EXPECT_GT(rate, 0) << "Expected positive throughput";
}

TEST_F(BenchTransport, RecordsPerSec2Producers) {
    int64_t rate = run_producers(2, 50'000);
    EXPECT_GT(rate, 0) << "Expected positive throughput";
}

TEST_F(BenchTransport, RecordsPerSec4Producers) {
    int64_t rate = run_producers(4, 25'000);
    EXPECT_GT(rate, 0) << "Expected positive throughput";
}

TEST_F(BenchTransport, RecordsPerSec8Producers) {
    int64_t rate = run_producers(8, 12'500);
    EXPECT_GT(rate, 0) << "Expected positive throughput";
}

}  // namespace
}  // namespace lumen
