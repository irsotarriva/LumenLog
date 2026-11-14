#include "delivery.hpp"

#include <chrono>
#include <concepts>
#include <cstdint>
#include <exception>
#include <memory>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>

#include "lumen/predicate.hpp"
#include "lumen/sink.hpp"

namespace lumen::detail {

namespace {

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

template <RecordType R>
constexpr std::string_view callback_name = "on_log";
template <>
constexpr std::string_view callback_name<MetricRecord> = "on_metric";
template <>
constexpr std::string_view callback_name<ProgressRecord> = "on_progress";

// Runs one callback. A sink that is already busy is in a callback further up
// this thread's stack (only a fatal record or a shutdown re-enters
// delivery), so it is skipped. Exceptions are contained here; e.what() is
// read inside the handler because it dies with the exception.
template <std::invocable F>
void call_sink(SinkEntry& entry, std::string_view callback, FirstReports& reports,
               F&& fn) noexcept {
    if (entry.busy.exchange(true, std::memory_order_seq_cst)) {
        return;
    }
    if (entry.removed.load(std::memory_order_seq_cst)) {
        entry.busy.store(false, std::memory_order_release);
        return;
    }
    try {
        std::forward<F>(fn)();
    } catch (const std::exception& e) {
        reports.sink_threw(entry, callback, e.what());
    } catch (...) {
        reports.sink_threw(entry, callback, "unknown exception");
    }
    entry.busy.store(false, std::memory_order_release);
}

template <RecordType R>
void offer_to_remaining_sinks(const R& record, InFlight& in_flight, FirstReports& reports) {
    while (in_flight.sinks != nullptr && in_flight.next < in_flight.sinks->size()) {
        SinkEntry& entry = *(*in_flight.sinks)[in_flight.next++];
        if (matches(entry.predicate, record)) {
            call_sink(entry, callback_name<R>, reports, [&] { deliver_to(*entry.sink, record); });
        }
    }
}

}  // namespace

// Cycles longer than this are not looked for; none of that length is
// expected between the Cores of one program.
inline constexpr uint32_t max_cycle_length = 64;

Delivery::Waiting::Waiting(const Delivery& target) noexcept : target_(&target) {
    for (DeliveryFrame* frame = innermost_delivery_frame(); frame != nullptr;
         frame = frame->outer) {
        frame->owner->waits_for_.store(&target, std::memory_order_seq_cst);
        marked_ = true;
    }
}

Delivery::Waiting::~Waiting() {
    for (DeliveryFrame* frame = innermost_delivery_frame(); frame != nullptr;
         frame = frame->outer) {
        frame->owner->waits_for_.store(nullptr, std::memory_order_seq_cst);
    }
}

bool Delivery::Waiting::in_cycle() const noexcept {
    const Delivery* current = target_;
    for (uint32_t step = 0; step < max_cycle_length; ++step) {
        const Delivery* const next = current->waits_for_.load(std::memory_order_seq_cst);
        if (next == nullptr) {
            return false;
        }
        if (delivery_frame_for(*next) != nullptr) {
            return true;
        }
        current = next;
    }
    return false;
}

bool Delivery::Session::try_lock_within_(std::unique_lock<std::mutex>& lock,
                                         std::chrono::milliseconds limit) {
    constexpr std::chrono::microseconds poll_interval{200};
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!lock.try_lock()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(poll_interval);
    }
    return true;
}

bool Delivery::delivering_here() const noexcept {
    return delivery_frame_for(*this) != nullptr;
}

void Delivery::reset_in_fork_child() noexcept {
    std::construct_at(&mutex_);
    waits_for_.store(nullptr, std::memory_order_relaxed);
    snapshot_loops_ = 0;
}

uint64_t Delivery::deliver_up_to(const QueuePositions& targets) {
    return queues_->pop_up_to(targets, [this](const auto& record) { dispatch_(&record); });
}

void Delivery::deliver_all_up_to(const QueuePositions& targets) {
    while (true) {
        // The count is not needed: progress is judged by the tails.
        static_cast<void>(deliver_up_to(targets));
        if (queues_->reached(targets)) {
            return;
        }
        std::this_thread::yield();
    }
}

void Delivery::deliver_everything_queued() {
    deliver_all_up_to(queues_->heads());
}

void Delivery::dispatch(const LogRecord& record) {
    dispatch_(&record);
}

void Delivery::flush_sinks() {
    const Registry& sinks = snapshot_();
    ++snapshot_loops_;
    for (const std::shared_ptr<SinkEntry>& entry : sinks) {
        call_sink(*entry, "flush", *reports_, [&] { entry->sink->flush(); });
        count_progress_();
    }
    --snapshot_loops_;
}

void Delivery::finish_in_flight() {
    DeliveryFrame* const frame = delivery_frame_for(*this);
    if (frame != nullptr) {
        continue_(frame->in_flight);
    }
}

void Delivery::release_snapshot() noexcept {
    snapshot_sinks_.reset();
}

void Delivery::dispatch_(InFlight::Record record) {
    DeliveryFrame* const frame = delivery_frame_for(*this);
    const InFlight outer = std::exchange(frame->in_flight, InFlight{record, &snapshot_(), 0});
    continue_(frame->in_flight);
    frame->in_flight = outer;
    count_progress_();
}

void Delivery::continue_(InFlight& in_flight) {
    ++snapshot_loops_;
    std::visit(
        [&]<typename P>(const P record) {
            if constexpr (!std::same_as<P, std::monostate>) {
                offer_to_remaining_sinks(*record, in_flight, *reports_);
            }
        },
        in_flight.record);
    in_flight.record = std::monostate{};
    --snapshot_loops_;
}

const Registry& Delivery::snapshot_() {
    if (snapshot_loops_ == 0 && registry_->version() != snapshot_version_) {
        // The replaced snapshot is released here, after the registry's lock:
        // it may hold the last reference to a removed sink's entry.
        const std::shared_ptr<const Registry> replaced =
            registry_->refresh(snapshot_sinks_, snapshot_version_);
    }
    return *snapshot_sinks_;
}

}  // namespace lumen::detail
