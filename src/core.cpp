#include "lumen/core.hpp"

#include <atomic>
#include <cstdint>
#include <exception>
#include <expected>
#include <format>
#include <memory>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>

#include "delivery.hpp"
#include "diagnostic_reporter.hpp"
#include "fatal_records.hpp"
#include "first_reports.hpp"
#include "immortal.hpp"
#include "record_preparation.hpp"
#include "record_queues.hpp"
#include "sink_registry.hpp"
#include "wakeup.hpp"

namespace lumen {

// Composes the parts of a Core (src/*.hpp, one responsibility each) and runs
// the sequences that span them: submission, the dispatch loop, flush, fatal
// records and shutdown.
struct Core::Impl {
public:
    explicit Impl(DiagnosticHandler handler) : reporter_(std::move(handler)) {
        start_dispatcher_();
    }
    ~Impl() { shut_down(); }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    // R is named explicitly by the caller, so `record` is an rvalue
    // reference, not a forwarding one. `source` matters for log records only.
    template <RecordType R>
    void submit(std::type_identity_t<R>&& record, uint32_t tags_dropped_explicit,
                SourceStrings source = SourceStrings::Static) {
        if (!accepting_.load(std::memory_order_acquire)) {
            queues_.count_after_shutdown<R>();
            return;
        }
        prepare_(record, tags_dropped_explicit, source);
        // A refused record is counted by its queue and reported once, after
        // the next delivery.
        static_cast<void>(queues_.push<R>(std::move(record)));
        if (delivers_on_submit_) {
            deliver_on_this_thread_();
            return;
        }
        wakeup_.wake_if_sleeping();
    }

    [[noreturn]] void submit_fatal(LogRecord& record, uint32_t tags_dropped_explicit,
                                   SourceStrings source);
    void flush();
    void shut_down() noexcept;

    [[nodiscard]] std::expected<SinkId, std::error_code> add_sink(std::unique_ptr<Sink> sink,
                                                                  Predicate predicate) {
        return registry_.add(std::move(sink), std::move(predicate));
    }
    [[nodiscard]] std::expected<std::unique_ptr<Sink>, std::error_code> remove_sink(SinkId id) {
        return registry_.remove(id, delivery_.delivering_here());
    }
    [[nodiscard]] std::expected<uint64_t, std::error_code> sink_exception_count(SinkId id) const {
        return registry_.exception_count(id);
    }
    [[nodiscard]] CoreStatistics statistics() const noexcept;
    void set_diagnostic_handler(DiagnosticHandler handler) {
        reporter_.set_handler(std::move(handler));
    }

private:
    template <RecordType R>
    void prepare_(R& record, uint32_t tags_dropped_explicit, SourceStrings source) {
        const uint32_t dropped =
            tags_dropped_explicit + detail::prepare_record(record, source == SourceStrings::Copied);
        if (dropped != 0) {
            tags_dropped_.fetch_add(dropped, std::memory_order_relaxed);
        }
    }

    void start_dispatcher_();
    void run_dispatcher_();
    void deliver_on_this_thread_();
    void shut_down_from_delivery_() noexcept;
    void report_first_losses_();

    detail::DiagnosticReporter reporter_;
    detail::FirstReports first_reports_{reporter_};
    detail::RecordQueues queues_;
    detail::Wakeup wakeup_;
    detail::SinkRegistry registry_;
    detail::Delivery delivery_{queues_, registry_, first_reports_};
    detail::FatalRecords fatals_;

    std::atomic<bool> accepting_{true};  // false once shut_down() began
    std::atomic<uint64_t> tags_dropped_{0};
    // Set by the constructor, before any other thread can see this Core.
    bool delivers_on_submit_ = false;
    std::thread dispatcher_;  // last: it starts in the constructor and uses everything above
};

// A Core whose dispatch thread could not be started is still usable: its
// records are delivered by the threads that submit them.
void Core::Impl::start_dispatcher_() {
    try {
        // std::thread, not std::jthread: Apple's libc++ does not provide jthread.
        dispatcher_ = std::thread([this] { run_dispatcher_(); });
    } catch (const std::exception& e) {
        delivers_on_submit_ = true;
        reporter_.report(
            std::format("lumen: the dispatch thread could not be started ({}); each record is "
                        "delivered on the thread that submits it",
                        e.what()));
    }
}

void Core::Impl::run_dispatcher_() {
    while (wakeup_.wait_for_work(queues_)) {
        uint64_t delivered = 0;
        {
            const detail::Delivery::Session session(delivery_);
            delivered = delivery_.deliver_up_to(queues_.heads());
        }
        report_first_losses_();
        // A claimed record that is not yet published cannot be popped; let
        // its producer run instead of spinning on it.
        if (delivered == 0) {
            std::this_thread::yield();
        }
    }
}

// Without a dispatch thread. A record submitted from inside a callback is
// left for the next submission or flush(): this thread is delivering
// already, further up its stack.
void Core::Impl::deliver_on_this_thread_() {
    if (delivery_.delivering_here()) {
        return;
    }
    {
        const detail::Delivery::Session session(delivery_);
        delivery_.deliver_everything_queued();
    }
    report_first_losses_();
}

void Core::Impl::flush() {
    if (delivery_.delivering_here() || !accepting_.load(std::memory_order_acquire)) {
        return;
    }
    const detail::QueuePositions targets = queues_.heads();
    {
        const detail::Delivery::Session session(delivery_);
        delivery_.deliver_all_up_to(targets);
        delivery_.flush_sinks();
    }
    report_first_losses_();
}

void Core::Impl::submit_fatal(LogRecord& record, uint32_t tags_dropped_explicit,
                              SourceStrings source) {
    if (!accepting_.load(std::memory_order_acquire)) {
        queues_.count_after_shutdown<LogRecord>();
        std::terminate();
    }
    fatals_.announce();
    prepare_(record, tags_dropped_explicit, source);
    fatals_.post(std::move(record));
    if (delivery_.delivering_here()) {
        // Inside a callback: this thread already holds a Session.
        fatals_.deliver_all(delivery_);
        report_first_losses_();
        std::terminate();
    }
    const detail::Delivery::Session session(delivery_);
    fatals_.deliver_all(delivery_);
    report_first_losses_();
    std::terminate();
}

void Core::Impl::shut_down() noexcept {
    if (!accepting_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    registry_.close();
    wakeup_.stop();
    if (delivery_.delivering_here()) {
        shut_down_from_delivery_();
        return;
    }
    if (dispatcher_.joinable()) {
        dispatcher_.join();
    }
    {
        const detail::Delivery::Session session(delivery_);
        delivery_.deliver_everything_queued();
        delivery_.flush_sinks();
        delivery_.release_snapshot();
    }
    report_first_losses_();
    registry_.destroy_sinks();
}

// Shutdown started inside a sink callback of this Core: the callback called
// exit(), so static destruction runs on the delivering thread. That thread
// holds the delivery lock further up its stack and never returns there, so
// it delivers what is left itself, cannot join the dispatcher (which may be
// this thread, or be waiting for that lock) and does not destroy the sinks
// (some are in a call on this stack).
void Core::Impl::shut_down_from_delivery_() noexcept {
    delivery_.finish_in_flight();
    delivery_.deliver_everything_queued();
    delivery_.flush_sinks();
    report_first_losses_();
    if (dispatcher_.joinable()) {
        dispatcher_.detach();
    }
}

void Core::Impl::report_first_losses_() {
    first_reports_.report_new_losses(queues_.refused_when_full(),
                                     tags_dropped_.load(std::memory_order_relaxed));
}

CoreStatistics Core::Impl::statistics() const noexcept {
    QueueStatistics logs = queues_.statistics<LogRecord>();
    logs.accepted += fatals_.announced();
    return CoreStatistics{logs, queues_.statistics<MetricRecord>(),
                          queues_.statistics<ProgressRecord>(),
                          tags_dropped_.load(std::memory_order_relaxed)};
}

Core::Core(DiagnosticHandler diagnostics) : impl_(std::make_unique<Impl>(std::move(diagnostics))) {}

// Shuts down before impl_ starts to be destroyed, so a sink destructor that
// calls back into this Core still finds impl_ (std::unique_ptr may clear its
// pointer before it deletes).
Core::~Core() {
    impl_->shut_down();
}

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

void Core::submit_(LogRecord&& record, uint32_t tags_dropped, SourceStrings source) noexcept {
    impl_->submit<LogRecord>(std::move(record), tags_dropped, source);
}

void Core::submit_(MetricRecord&& record, uint32_t tags_dropped) noexcept {
    impl_->submit<MetricRecord>(std::move(record), tags_dropped);
}

void Core::submit_(ProgressRecord&& record, uint32_t tags_dropped) noexcept {
    impl_->submit<ProgressRecord>(std::move(record), tags_dropped);
}

void Core::submit_fatal_(LogRecord& record, uint32_t tags_dropped, SourceStrings source) noexcept {
    impl_->submit_fatal(record, tags_dropped, source);
}

struct Core::ShutdownAtExit {
    explicit ShutdownAtExit(Core& target) : core(target) {}
    Core& core;
    ~ShutdownAtExit() { core.impl_->shut_down(); }
    ShutdownAtExit(const ShutdownAtExit&) = delete;
    ShutdownAtExit& operator=(const ShutdownAtExit&) = delete;
    ShutdownAtExit(ShutdownAtExit&&) = delete;
    ShutdownAtExit& operator=(ShutdownAtExit&&) = delete;
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
