// Wakeup (src/wakeup.hpp): how a dispatch thread waits for records and how
// producers wake it. The consumer side runs on a thread of the test, which
// stands in for the dispatcher.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <random>
#include <thread>

#include "lumen/record.hpp"
#include "record_queues.hpp"
#include "wakeup.hpp"

namespace lumen::detail {
namespace {

// Bounds a wait that only a defect makes long; a correct Wakeup returns in
// microseconds.
constexpr std::chrono::seconds generous_timeout{10};

void push_one_log(RecordQueues& queues) {
    ASSERT_TRUE(queues.push<LogRecord>(LogRecord{}));
}

// A log, a metric or a progress record, by `index` modulo 3.
void push_one_of_each_kind_in_turn(RecordQueues& queues, uint64_t index) {
    switch (index % 3) {
        case 0:  ASSERT_TRUE(queues.push<LogRecord>(LogRecord{})); break;
        case 1:  ASSERT_TRUE(queues.push<MetricRecord>(MetricRecord{})); break;
        default: ASSERT_TRUE(queues.push<ProgressRecord>(ProgressRecord{})); break;
    }
}

// The dispatcher's question: is there a record to deliver?
[[nodiscard]] auto pending_in(const RecordQueues& queues) {
    return [&queues] { return queues.has_pending(); };
}

[[nodiscard]] uint64_t pop_everything(RecordQueues& queues) {
    return queues.pop_up_to(queues.heads(), [](const auto& /*record*/) {});
}

// Runs wait_for_work() once on its own thread. If a test fails before the
// wait returns, the destructor stops the Wakeup so that the thread can end.
class OneWait {
public:
    OneWait(Wakeup& wakeup, const RecordQueues& queues) : wakeup_(&wakeup) {
        std::packaged_task<bool()> task(
            [&wakeup, &queues] { return wakeup.wait_for_work(pending_in(queues)); });
        result_ = task.get_future();
        // std::thread, not std::jthread: Apple's libc++ does not provide jthread.
        thread_ = std::thread(std::move(task));
    }
    ~OneWait() {
        wakeup_->stop();
        thread_.join();
    }
    OneWait(const OneWait&) = delete;
    OneWait& operator=(const OneWait&) = delete;
    OneWait(OneWait&&) = delete;
    OneWait& operator=(OneWait&&) = delete;

    [[nodiscard]] bool returned_within(std::chrono::seconds timeout) const {
        return result_.wait_for(timeout) == std::future_status::ready;
    }
    [[nodiscard]] bool result() { return result_.get(); }

private:
    Wakeup* wakeup_;  // non-owning: the test's
    std::future<bool> result_;
    std::thread thread_;
};

// Spins for a random time of up to `max`. Picks a point in time on this
// thread without handing the choice to the scheduler, as a sleep would.
void busy_wait_up_to(std::mt19937& random, std::chrono::microseconds max) {
    std::uniform_int_distribution<int64_t> delay_us(0, max.count());
    const auto until =
        std::chrono::steady_clock::now() + std::chrono::microseconds(delay_us(random));
    while (std::chrono::steady_clock::now() < until) {
        // see above
    }
}

TEST(WakeupTest, WaitWithNothingQueuedReturnsFalseWhenStoppedAtAnyPointOfIt) {
    // A wait that returned true with nothing queued would make the
    // dispatcher loop without work, never sleeping. The consumer looks at
    // the queues for about a millisecond before it blocks; stopping it at
    // random points of that time and after it covers both.
    constexpr int32_t rounds = 50;
    // NOLINTNEXTLINE(cert-msc32-c,cert-msc51-cpp): a fixed seed makes a failing round reproducible
    std::mt19937 random(20261111);
    for (int32_t round = 0; round < rounds; ++round) {
        const RecordQueues queues;
        Wakeup wakeup;
        OneWait wait(wakeup, queues);
        busy_wait_up_to(random, std::chrono::microseconds(2000));
        wakeup.stop();
        ASSERT_TRUE(wait.returned_within(generous_timeout));
        EXPECT_FALSE(wait.result()) << "round " << round;
    }
}

TEST(WakeupTest, WaitAfterStopReturnsFalseEvenWithARecordQueued) {
    RecordQueues queues;
    Wakeup wakeup;
    push_one_log(queues);
    wakeup.stop();
    EXPECT_FALSE(wakeup.wait_for_work(pending_in(queues)));
}

TEST(WakeupTest, WaitWithARecordAlreadyQueuedReturnsTrueWithoutAWake) {
    RecordQueues queues;
    Wakeup wakeup;
    push_one_log(queues);
    EXPECT_TRUE(wakeup.wait_for_work(pending_in(queues)));
}

TEST(WakeupTest, RecordPushedAndWokenWhileTheConsumerWaitsEndsTheWaitWithTrue) {
    RecordQueues queues;
    Wakeup wakeup;
    OneWait wait(wakeup, queues);
    push_one_log(queues);
    wakeup.wake_if_sleeping();
    ASSERT_TRUE(wait.returned_within(generous_timeout)) << "the wake-up was lost";
    EXPECT_TRUE(wait.result());
}

// How many times a wait with nothing queued checks for work before it
// blocks: stopped once it has checked, it makes all of its checks anyway.
[[nodiscard]] uint32_t checks_before_sleeping() {
    const RecordQueues queues;
    Wakeup wakeup;
    std::atomic<uint32_t> checks{0};
    // std::thread, not std::jthread: Apple's libc++ does not provide jthread.
    std::thread consumer([&] {
        static_cast<void>(wakeup.wait_for_work([&] {
            checks.fetch_add(1);
            return queues.has_pending();
        }));
    });
    while (checks.load() == 0) {
        std::this_thread::yield();
    }
    wakeup.stop();
    consumer.join();
    return checks.load();
}

TEST(WakeupTest, RecordPushedDuringAnyCheckBeforeSleepingEndsTheWaitWithTrue) {
    // A producer publishes its record and calls wake_if_sleeping() while the
    // consumer is checking the queues on its way to sleep: the record is
    // pushed from inside the consumer's k-th check, after that check found
    // nothing, for every check the consumer makes. Each wait must end with
    // true; one still waiting is a lost wake-up (the record stays queued and
    // no further push follows).
    const uint32_t checks_per_wait = checks_before_sleeping();
    ASSERT_GT(checks_per_wait, 0U);
    for (uint32_t k = 1; k <= checks_per_wait; ++k) {
        RecordQueues queues;
        Wakeup wakeup;
        uint32_t checks = 0;  // used by the consumer thread only
        const auto has_work = [&] {
            const bool pending = queues.has_pending();
            if (++checks == k && !pending) {
                EXPECT_TRUE(queues.push<LogRecord>(LogRecord{}));
                wakeup.wake_if_sleeping();
            }
            return pending;
        };
        std::packaged_task<bool()> task([&] { return wakeup.wait_for_work(has_work); });
        std::future<bool> result = task.get_future();
        std::thread consumer(std::move(task));
        const bool ended = result.wait_for(generous_timeout) == std::future_status::ready;
        if (!ended) {
            wakeup.stop();  // so that the consumer ends and can be joined
        }
        consumer.join();
        EXPECT_TRUE(ended) << "a record pushed during check " << k << " was never taken";
        EXPECT_TRUE(result.get()) << "check " << k;
    }
}

// The consumer side of the stress test below: waits, empties the queues and
// counts the records, until stopped.
class Consumer {
public:
    Consumer(Wakeup& wakeup, RecordQueues& queues)
        : thread_([this, &wakeup, &queues] { run_(wakeup, queues); }) {}
    ~Consumer() { thread_.join(); }
    Consumer(const Consumer&) = delete;
    Consumer& operator=(const Consumer&) = delete;
    Consumer(Consumer&&) = delete;
    Consumer& operator=(Consumer&&) = delete;

    // Whether the consumer has taken `count` records within `timeout`.
    [[nodiscard]] bool wait_until_taken(uint64_t count, std::chrono::seconds timeout) {
        std::unique_lock lock(mutex_);
        return taken_cv_.wait_for(lock, timeout, [&] { return taken_ >= count; });
    }

private:
    void run_(Wakeup& wakeup, RecordQueues& queues) {
        while (wakeup.wait_for_work(pending_in(queues))) {
            const uint64_t popped = pop_everything(queues);
            {
                const std::scoped_lock lock(mutex_);
                taken_ += popped;
            }
            taken_cv_.notify_all();
        }
    }

    std::mutex mutex_;
    std::condition_variable taken_cv_;  // waits on mutex_
    uint64_t taken_ = 0;                // guarded by mutex_
    std::thread thread_;  // last: it starts in the constructor and uses the members above
};

TEST(WakeupTest, EveryRecordPushedWhileTheConsumerGoesToSleepIsTakenWithoutAnotherPush) {
    // One record at a time (logs, metrics and progress records in turn),
    // each pushed at a random point of the consumer's way from its last
    // record to sleep (it checks the queues for about a millisecond first).
    // A lost wake-up leaves that record queued forever, since no further
    // push follows it. The delay is a busy wait, so the point in time is
    // chosen by this thread, not by the scheduler.
    constexpr uint64_t rounds = 2000;
    constexpr std::chrono::microseconds max_delay{1500};
    RecordQueues queues;
    Wakeup wakeup;
    // NOLINTNEXTLINE(cert-msc32-c,cert-msc51-cpp): a fixed seed makes a failing round reproducible
    std::mt19937 random(20261111);
    {
        Consumer consumer(wakeup, queues);
        for (uint64_t round = 1; round <= rounds; ++round) {
            busy_wait_up_to(random, max_delay);
            push_one_of_each_kind_in_turn(queues, round);
            wakeup.wake_if_sleeping();
            if (!consumer.wait_until_taken(round, generous_timeout)) {
                ADD_FAILURE() << "record " << round << " was never taken: a lost wake-up";
                break;
            }
        }
        wakeup.stop();
    }
}

}  // namespace
}  // namespace lumen::detail
