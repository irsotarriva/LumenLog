#include "lumen/core.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <exception>
#include <format>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "immortal.hpp"
#include "lumen/detail/context.hpp"
#include "lumen/detail/ring_buffer.hpp"
#include "lumen/error.hpp"
#include "lumen/limits.hpp"
#include "lumen/thread_clock.hpp"

namespace lumen {

namespace {

using LogQueue = RingBuffer<LogRecord, log_queue_capacity>;
using MetricQueue = RingBuffer<MetricRecord, metric_queue_capacity>;
using ProgressQueue = RingBuffer<ProgressRecord, progress_queue_capacity>;

template <typename R>
concept AnyRecord =
    std::same_as<R, LogRecord> || std::same_as<R, MetricRecord> || std::same_as<R, ProgressRecord>;

// One registered sink. Entries are shared between the registry and the
// snapshots the delivering thread holds, so an entry outlives its removal
// until no snapshot refers to it.
//
// Removal handshake (busy/removed, both sequentially consistent): the
// delivering thread sets busy and then reads removed before each callback;
// remove_sink sets removed and then waits while busy. Either the deliverer
// sees removed and skips the sink, or remove_sink sees busy and waits for the
// callback to end. After that, nothing reads `sink` again, and `owner` can be
// handed back.
struct SinkEntry {
    SinkEntry(SinkId id_in, std::unique_ptr<Sink> owner_in, Predicate predicate_in)
        : id(id_in)
        , owner(std::move(owner_in))
        , sink(owner.get())
        , predicate(std::move(predicate_in)) {}

    const SinkId id;
    std::unique_ptr<Sink> owner;  // moved out by remove_sink once no callback can run
    Sink* const sink;             // non-owning: owner.get() at registration
    const Predicate predicate;
    std::atomic<bool> busy{false};     // written only by the delivering thread
    std::atomic<bool> removed{false};  // written only by remove_sink
    std::atomic<uint64_t> exceptions{0};
};

using Registry = std::vector<std::shared_ptr<SinkEntry>>;

// What the calling thread is delivering, if anything: the Core whose records
// it delivers (it holds that Core's consume mutex) and the sink whose
// callback is running. Lets flush, remove_sink and fatal records recognise a
// call from inside a callback.
struct Delivery {
    const void* core = nullptr;  // non-owning: the Core::Impl
    SinkEntry* entry = nullptr;  // non-owning
};

thread_local Delivery tls_delivery;

class DeliveryScope {
public:
    explicit DeliveryScope(const void* core) noexcept
        : previous_(std::exchange(tls_delivery, Delivery{core, nullptr})) {}
    ~DeliveryScope() { tls_delivery = previous_; }
    DeliveryScope(const DeliveryScope&) = delete;
    DeliveryScope& operator=(const DeliveryScope&) = delete;
    DeliveryScope(DeliveryScope&&) = delete;
    DeliveryScope& operator=(DeliveryScope&&) = delete;

private:
    Delivery previous_;
};

[[nodiscard]] std::string_view& text_of(LogRecord& r) {
    return r.message;
}
[[nodiscard]] std::string_view& text_of(MetricRecord& r) {
    return r.name;
}
[[nodiscard]] std::string_view& text_of(ProgressRecord& r) {
    return r.label;
}

[[nodiscard]] bool matches(const Predicate& p, const LogRecord& r) {
    return p.evaluate(r.tags, r.level);
}
[[nodiscard]] bool matches(const Predicate& p, const MetricRecord& r) {
    return p.evaluate(r.tags);
}
[[nodiscard]] bool matches(const Predicate& p, const ProgressRecord& r) {
    return p.evaluate(r.tags);
}

void deliver_to(Sink& s, const LogRecord& r) {
    s.on_log(r);
}
void deliver_to(Sink& s, const MetricRecord& r) {
    s.on_metric(r);
}
void deliver_to(Sink& s, const ProgressRecord& r) {
    s.on_progress(r);
}

template <AnyRecord R>
constexpr std::string_view callback_name = "on_log";
template <>
constexpr std::string_view callback_name<MetricRecord> = "on_metric";
template <>
constexpr std::string_view callback_name<ProgressRecord> = "on_progress";

// Copies the record's text and every tag string into one NUL-terminated
// block and re-points the views at it.
template <AnyRecord R>
void own_strings(R& record) {
    std::string_view& text = text_of(record);
    std::size_t total = text.size() + 1;
    for (const auto& e : record.tags) {
        total += e.key.size() + 1 + e.value.size() + 1;
    }
    auto block = std::make_shared_for_overwrite<char[]>(total);
    char* out = block.get();
    auto copy = [&out](std::string_view s) {
        if (!s.empty()) std::memcpy(out, s.data(), s.size());
        out[s.size()] = '\0';
        const std::string_view owned(out, s.size());
        out += s.size() + 1;
        return owned;
    };
    text = copy(text);
    record.tags.remap(copy);
    record.storage = std::move(block);
}

}  // namespace

DiagnosticHandler stderr_diagnostics() {
    return [](std::string_view line) { std::cerr << line << '\n' << std::flush; };
}

// See Core::Impl::wait_for_work().
constexpr std::chrono::microseconds idle_check_interval{100};
constexpr int idle_check_count = 5;

struct Core::Impl {
    explicit Impl(DiagnosticHandler handler) : diagnostics(std::move(handler)) {
        // std::thread, not std::jthread: Apple's libc++ does not provide jthread.
        dispatcher = std::thread([this] { run_dispatcher(); });
    }
    ~Impl() { shut_down(); }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    template <AnyRecord R>
    [[nodiscard]] auto& queue() {
        if constexpr (std::same_as<R, LogRecord>)
            return logs;
        else if constexpr (std::same_as<R, MetricRecord>)
            return metrics;
        else
            return progress;
    }

    template <AnyRecord R>
    [[nodiscard]] std::atomic<uint64_t>& after_shutdown() {
        if constexpr (std::same_as<R, LogRecord>)
            return logs_after_shutdown;
        else if constexpr (std::same_as<R, MetricRecord>)
            return metrics_after_shutdown;
        else
            return progress_after_shutdown;
    }

    template <AnyRecord R>
    void finalize(R& record, uint32_t tags_dropped_explicit) {
        record.timestamp_ns = now_ns();
        record.thread_id = this_thread_id();
        const uint32_t dropped = tags_dropped_explicit + detail::merge_context(record.tags);
        if (dropped != 0) tags_dropped.fetch_add(dropped, std::memory_order_relaxed);
        own_strings(record);
    }

    template <AnyRecord R>
    void submit(R&& record, uint32_t tags_dropped_explicit) {
        if (!accepting.load(std::memory_order_acquire)) {
            after_shutdown<R>().fetch_add(1, std::memory_order_relaxed);
            return;
        }
        finalize(record, tags_dropped_explicit);
        // A refused record is counted by the queue (rejected_count) and
        // reported once by the dispatcher.
        static_cast<void>(queue<R>().try_push(std::move(record)));
        wake_if_sleeping();
    }

    // Wake-up protocol. Both sides update `sleeping` with a read-modify-write
    // (acq_rel), so the two updates are ordered in its modification order and
    // the later one sees the earlier:
    //   - the dispatcher sets it with an exchange, then checks the queues once
    //     more, and only then blocks;
    //   - a producer, after publishing its record, clears it with an exchange
    //     and notifies if it was set.
    // If the producer's exchange comes second, it sees `true` and wakes the
    // dispatcher. If it comes first, the dispatcher's exchange reads from it
    // (acquire), so the record published before it is visible to the second
    // check. Either way no record waits for a sleeping dispatcher. (A
    // seq_cst fence on each side would do the same, but ThreadSanitizer does
    // not support fences.)
    // Cost on every emit: one atomic exchange on a shared cache line (a
    // locked instruction, a few tens of cycles; producers on several cores
    // also move that line between them). The mutex and the notify (a system
    // call) are only paid by a producer that finds the dispatcher asleep.
    void wake_if_sleeping() noexcept {
        if (!sleeping.exchange(false, std::memory_order_acq_rel)) return;
        {
            // Empty critical section: the dispatcher tests `sleeping` and
            // starts waiting under wake_mutex, so after this the notify
            // cannot fall between its test and its wait.
            const std::scoped_lock lock(wake_mutex);
        }
        wake_cv.notify_one();
    }

    [[nodiscard]] bool has_pending() const noexcept {
        return logs.head_position() != logs.tail_position() ||
               metrics.head_position() != metrics.tail_position() ||
               progress.head_position() != progress.tail_position();
    }

    // Returns false when the Core is stopping.
    //
    // Before blocking, a dispatcher that has just delivered records checks
    // the queues a few more times, idle_check_interval apart. Records tend to
    // come in bursts; a dispatcher that blocked as soon as it caught up would
    // make the producer of the next record pay for waking it (a system call),
    // over and over within one burst. Sleeping between the checks, rather
    // than spinning, keeps it off the producers' cache lines meanwhile. An
    // idle dispatcher wakes up only these few times after its last record.
    [[nodiscard]] bool wait_for_work() {
        if (stopping.load(std::memory_order_acquire)) return false;
        for (int check = 0; check < idle_check_count && !has_pending(); ++check) {
            std::this_thread::sleep_for(idle_check_interval);
        }
        if (has_pending()) return true;
        static_cast<void>(sleeping.exchange(true, std::memory_order_acq_rel));
        if (has_pending()) {
            sleeping.store(false, std::memory_order_relaxed);
            return true;
        }
        std::unique_lock lock(wake_mutex);
        wake_cv.wait(lock, [this] {
            return !sleeping.load(std::memory_order_relaxed) ||
                   stopping.load(std::memory_order_relaxed);
        });
        sleeping.store(false, std::memory_order_relaxed);
        return !stopping.load(std::memory_order_relaxed);
    }

    void run_dispatcher() {
        while (wait_for_work()) {
            uint64_t delivered = 0;
            {
                const std::scoped_lock consume(consume_mutex);
                const DeliveryScope scope(this);
                delivered = deliver_up_to(logs.head_position(), metrics.head_position(),
                                          progress.head_position());
            }
            report_first_losses();
            // A claimed record that is not yet published cannot be popped;
            // let its producer run instead of spinning on it.
            if (delivered == 0) std::this_thread::yield();
        }
    }

    // Caller holds consume_mutex. Delivers records until each queue's tail
    // reaches its target or the next record is not yet published. Returns
    // how many were delivered.
    [[nodiscard]] uint64_t deliver_up_to(uint64_t log_target, uint64_t metric_target,
                                         uint64_t progress_target) {
        return deliver_queue(progress, progress_target) + deliver_queue(metrics, metric_target) +
               deliver_queue(logs, log_target);
    }

    template <AnyRecord R, std::size_t N>
    [[nodiscard]] uint64_t deliver_queue(RingBuffer<R, N>& q, uint64_t target) {
        uint64_t delivered = 0;
        R record;
        while (q.tail_position() < target && q.try_pop(record)) {
            dispatch(record);
            ++delivered;
        }
        return delivered;
    }

    // Caller holds consume_mutex. Waits for producers that claimed a slot
    // before the targets were read to publish it.
    void deliver_all_up_to(uint64_t log_target, uint64_t metric_target, uint64_t progress_target) {
        while (true) {
            static_cast<void>(deliver_up_to(log_target, metric_target, progress_target));
            if (logs.tail_position() >= log_target && metrics.tail_position() >= metric_target &&
                progress.tail_position() >= progress_target) {
                return;
            }
            std::this_thread::yield();
        }
    }

    void deliver_everything_queued() {
        deliver_all_up_to(logs.head_position(), metrics.head_position(), progress.head_position());
    }

    // Caller holds consume_mutex. The registry as the delivering thread sees
    // it: a snapshot refreshed when registry_version moves, so delivering a
    // record takes no lock. A callback that adds or removes a sink replaces
    // `registry`, not this snapshot, so the loop over the snapshot that is
    // running the callback stays valid.
    [[nodiscard]] const Registry& delivery_registry() {
        if (registry_version.load(std::memory_order_acquire) != delivery_version) {
            const std::scoped_lock lock(registry_mutex);
            delivery_snapshot = registry;
            delivery_version = registry_version.load(std::memory_order_relaxed);
        }
        return *delivery_snapshot;
    }

    template <AnyRecord R>
    void dispatch(const R& record) {
        for (const std::shared_ptr<SinkEntry>& entry : delivery_registry()) {
            if (!matches(entry->predicate, record)) continue;
            call_sink(*entry, callback_name<R>, [&] { deliver_to(*entry->sink, record); });
        }
    }

    void flush_sinks() {
        for (const std::shared_ptr<SinkEntry>& entry : delivery_registry()) {
            call_sink(*entry, "flush", [&] { entry->sink->flush(); });
        }
    }

    // Runs one callback. A sink that is already busy is in a callback further
    // up this thread's stack (only a fatal record re-enters delivery), so it
    // is skipped. Exceptions are contained here; e.what() is read inside the
    // handler because it dies with the exception.
    template <std::invocable F>
    void call_sink(SinkEntry& entry, std::string_view callback, F&& fn) noexcept {
        if (entry.busy.exchange(true, std::memory_order_seq_cst)) return;
        if (entry.removed.load(std::memory_order_seq_cst)) {
            entry.busy.store(false, std::memory_order_release);
            return;
        }
        SinkEntry* const outer = std::exchange(tls_delivery.entry, &entry);
        try {
            std::forward<F>(fn)();
        } catch (const std::exception& e) {
            note_exception(entry, callback, e.what());
        } catch (...) {
            note_exception(entry, callback, "unknown exception");
        }
        tls_delivery.entry = outer;
        entry.busy.store(false, std::memory_order_release);
    }

    void note_exception(SinkEntry& entry, std::string_view callback, std::string_view what) {
        if (entry.exceptions.fetch_add(1, std::memory_order_relaxed) == 0) {
            report(
                std::format("lumen: sink {} threw from {}: {} (further exceptions from this "
                            "sink are only counted)",
                            entry.id, callback, what));
        }
    }

    void report(std::string_view line) {
        DiagnosticHandler handler;
        {
            const std::scoped_lock lock(diagnostics_mutex);
            handler = diagnostics;
        }
        // Called without the lock, so a handler may replace itself.
        if (handler) handler(line);
    }

    void report_first_losses() {
        const uint64_t refused =
            logs.rejected_count() + metrics.rejected_count() + progress.rejected_count();
        if (refused != 0 && !reported_queue_full.exchange(true, std::memory_order_relaxed)) {
            report(
                "lumen: a record was dropped because its queue was full; further drops are "
                "only counted (Core::statistics())");
        }
        if (tags_dropped.load(std::memory_order_relaxed) != 0 &&
            !reported_tags_dropped.exchange(true, std::memory_order_relaxed)) {
            report(
                "lumen: a tag was dropped because its record already had the maximum number "
                "of tags; further drops are only counted (Core::statistics())");
        }
    }

    [[nodiscard]] bool delivering_here() const noexcept { return tls_delivery.core == this; }

    void flush() {
        if (delivering_here() || !accepting.load(std::memory_order_acquire)) return;
        const uint64_t log_target = logs.head_position();
        const uint64_t metric_target = metrics.head_position();
        const uint64_t progress_target = progress.head_position();
        {
            const std::scoped_lock consume(consume_mutex);
            const DeliveryScope scope(this);
            deliver_all_up_to(log_target, metric_target, progress_target);
            flush_sinks();
        }
        report_first_losses();
    }

    void deliver_fatal(const LogRecord& record) {
        deliver_everything_queued();
        dispatch(record);
        flush_sinks();
    }

    void submit_fatal(LogRecord&& record, uint32_t tags_dropped_explicit) {
        if (!accepting.load(std::memory_order_acquire)) {
            logs_after_shutdown.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        finalize(record, tags_dropped_explicit);
        if (delivering_here()) {
            // Inside a callback: this thread already holds consume_mutex.
            deliver_fatal(record);
            return;
        }
        const std::scoped_lock consume(consume_mutex);
        const DeliveryScope scope(this);
        deliver_fatal(record);
    }

    void shut_down() noexcept {
        if (!accepting.exchange(false, std::memory_order_acq_rel)) return;
        {
            const std::scoped_lock lock(wake_mutex);
            stopping.store(true, std::memory_order_release);
        }
        wake_cv.notify_one();
        if (dispatcher.joinable()) dispatcher.join();
        {
            const std::scoped_lock consume(consume_mutex);
            const DeliveryScope scope(this);
            deliver_everything_queued();
            flush_sinks();
            delivery_snapshot.reset();
        }
        report_first_losses();
        // Destroys the sinks: no snapshot refers to them any more.
        const std::scoped_lock lock(registry_mutex);
        replace_registry(std::make_shared<const Registry>());
    }

    // Caller holds registry_mutex.
    void replace_registry(std::shared_ptr<const Registry> next) {
        registry = std::move(next);
        registry_version.fetch_add(1, std::memory_order_release);
    }

    [[nodiscard]] std::expected<SinkId, std::error_code> add_sink(std::unique_ptr<Sink> sink,
                                                                  Predicate predicate) {
        if (!sink) return std::unexpected(make_error_code(LumenError::NullSink));
        const SinkId id = next_sink_id.fetch_add(1, std::memory_order_relaxed);
        auto entry = std::make_shared<SinkEntry>(id, std::move(sink), std::move(predicate));
        const std::scoped_lock lock(registry_mutex);
        auto next = std::make_shared<Registry>(*registry);
        next->push_back(std::move(entry));
        replace_registry(std::move(next));
        return id;
    }

    [[nodiscard]] std::expected<std::unique_ptr<Sink>, std::error_code> remove_sink(SinkId id) {
        std::shared_ptr<SinkEntry> entry;
        {
            const std::scoped_lock lock(registry_mutex);
            const auto it = std::ranges::find_if(
                *registry, [id](const std::shared_ptr<SinkEntry>& e) { return e->id == id; });
            if (it == registry->end()) {
                return std::unexpected(make_error_code(LumenError::InvalidSinkId));
            }
            if (delivering_here() && tls_delivery.entry == it->get()) {
                return std::unexpected(make_error_code(LumenError::SinkBusy));
            }
            entry = *it;
            entry->removed.store(true, std::memory_order_seq_cst);
            auto next = std::make_shared<Registry>(*registry);
            std::erase(*next, entry);
            replace_registry(std::move(next));
        }
        // Only a callback on another thread can be running; wait for it.
        while (entry->busy.load(std::memory_order_seq_cst))
            std::this_thread::yield();
        return std::move(entry->owner);
    }

    [[nodiscard]] std::expected<uint64_t, std::error_code> sink_exception_count(SinkId id) const {
        const std::scoped_lock lock(registry_mutex);
        for (const std::shared_ptr<SinkEntry>& entry : *registry) {
            if (entry->id == id) return entry->exceptions.load(std::memory_order_relaxed);
        }
        return std::unexpected(make_error_code(LumenError::InvalidSinkId));
    }

    [[nodiscard]] CoreStatistics statistics() const noexcept {
        auto of = [](const auto& q, const std::atomic<uint64_t>& after) {
            return QueueStatistics{q.head_position(), q.rejected_count(),
                                   after.load(std::memory_order_relaxed)};
        };
        return CoreStatistics{of(logs, logs_after_shutdown), of(metrics, metrics_after_shutdown),
                              of(progress, progress_after_shutdown),
                              tags_dropped.load(std::memory_order_relaxed)};
    }

    void set_diagnostic_handler(DiagnosticHandler handler) {
        const std::scoped_lock lock(diagnostics_mutex);
        diagnostics = std::move(handler);
    }

    LogQueue logs;
    MetricQueue metrics;
    ProgressQueue progress;

    std::atomic<bool> accepting{true};  // false once shut_down() began
    std::atomic<uint64_t> logs_after_shutdown{0};
    std::atomic<uint64_t> metrics_after_shutdown{0};
    std::atomic<uint64_t> progress_after_shutdown{0};
    std::atomic<uint64_t> tags_dropped{0};

    std::mutex wake_mutex;
    std::condition_variable wake_cv;    // waits on wake_mutex
    std::atomic<bool> sleeping{false};  // see wake_if_sleeping()
    std::atomic<bool> stopping{false};  // set under wake_mutex

    // Held by whichever thread delivers records (the dispatcher, flush(), a
    // fatal record, shutdown): the queues have a single consumer, and sinks
    // are never called concurrently.
    std::mutex consume_mutex;

    // Held only to read or replace `registry`, never during a callback.
    mutable std::mutex registry_mutex;
    std::shared_ptr<const Registry> registry =
        std::make_shared<const Registry>();             // guarded by registry_mutex
    std::atomic<uint64_t> registry_version{0};          // bumped under registry_mutex
    std::shared_ptr<const Registry> delivery_snapshot;  // guarded by consume_mutex
    uint64_t delivery_version = std::numeric_limits<uint64_t>::max();  // guarded by consume_mutex
    std::atomic<SinkId> next_sink_id{1};

    std::mutex diagnostics_mutex;
    DiagnosticHandler diagnostics;  // guarded by diagnostics_mutex
    std::atomic<bool> reported_queue_full{false};
    std::atomic<bool> reported_tags_dropped{false};

    std::thread dispatcher;  // last: it starts in the constructor and uses everything above
};

Core::Core(DiagnosticHandler diagnostics) : impl_(std::make_unique<Impl>(std::move(diagnostics))) {}

Core::~Core() = default;

std::expected<SinkId, std::error_code> Core::add_sink(std::unique_ptr<Sink> sink,
                                                      Predicate predicate) {
    return impl_->add_sink(std::move(sink), std::move(predicate));
}

std::expected<std::unique_ptr<Sink>, std::error_code> Core::remove_sink(SinkId id) {
    return impl_->remove_sink(id);
}

std::expected<uint64_t, std::error_code> Core::sink_exception_count(SinkId id) const {
    return impl_->sink_exception_count(id);
}

void Core::flush() {
    impl_->flush();
}

CoreStatistics Core::statistics() const noexcept {
    return impl_->statistics();
}

void Core::set_diagnostic_handler(DiagnosticHandler handler) {
    impl_->set_diagnostic_handler(std::move(handler));
}

void Core::submit_(LogRecord&& record, uint32_t tags_dropped) noexcept {
    impl_->submit(std::move(record), tags_dropped);
}

void Core::submit_(MetricRecord&& record, uint32_t tags_dropped) noexcept {
    impl_->submit(std::move(record), tags_dropped);
}

void Core::submit_(ProgressRecord&& record, uint32_t tags_dropped) noexcept {
    impl_->submit(std::move(record), tags_dropped);
}

void Core::submit_fatal_(LogRecord&& record, uint32_t tags_dropped) noexcept {
    impl_->submit_fatal(std::move(record), tags_dropped);
    std::terminate();
}

struct Core::ShutdownAtExit {
    Core& core;
    ~ShutdownAtExit() { core.impl_->shut_down(); }
};

Core& core() {
    // Never destroyed, so it is still a valid object when code runs during
    // static destruction; `shutdown` is destroyed where a plain function-local
    // static Core would have been, and shuts it down at that point.
    static detail::Immortal<Core> instance;
    static const Core::ShutdownAtExit shutdown{instance.get()};
    return instance.get();
}

}  // namespace lumen
