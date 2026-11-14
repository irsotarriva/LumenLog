#include "lumen/core.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "delivery.hpp"
#include "diagnostic_reporter.hpp"
#include "dispatcher_thread.hpp"
#include "fatal_records.hpp"
#include "first_reports.hpp"
#include "flush_requests.hpp"
#include "fork_handlers.hpp"
#include "lumen/detail/immortal.hpp"
#include "lumen/error.hpp"
#include "record_preparation.hpp"
#include "record_queues.hpp"
#include "sink_registry.hpp"
#include "termination.hpp"
#include "wakeup.hpp"

namespace lumen {

namespace {

using Clock = std::chrono::steady_clock;

// How often a bounded wait for another thread (shutdown waiting for the
// dispatch thread, a fatal record or shutdown waiting for the delivery lock)
// looks whether delivery still makes progress.
constexpr std::chrono::milliseconds stall_poll_interval{10};

// Decides when a thread that delivers has stopped making progress: when
// Delivery::progress() has not moved for `limit`.
class StallWatch {
public:
    StallWatch(std::chrono::milliseconds limit, const detail::Delivery& delivery)
        : limit_(limit)
        , delivery_(&delivery)
        , last_progress_(delivery.progress())
        , last_change_(Clock::now()) {}

    [[nodiscard]] bool stalled() {
        const uint64_t progress = delivery_->progress();
        const Clock::time_point now = Clock::now();
        if (progress != last_progress_) {
            last_progress_ = progress;
            last_change_ = now;
            return false;
        }
        return now - last_change_ >= limit_;
    }

private:
    std::chrono::milliseconds limit_;
    const detail::Delivery* delivery_;  // non-owning
    uint64_t last_progress_;
    Clock::time_point last_change_;
};

// `timeout` from now, or no deadline when that is beyond the clock's range.
[[nodiscard]] Clock::time_point deadline_after(std::chrono::milliseconds timeout) {
    const Clock::time_point now = Clock::now();
    if (timeout <= std::chrono::milliseconds{0}) {
        return now;
    }
    if (timeout >=
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::time_point::max() - now)) {
        return Clock::time_point::max();
    }
    return now + timeout;
}

}  // namespace

// Composes the parts of a Core (src/*.hpp, one responsibility each) and runs
// the sequences that span them: submission, the dispatch loop, flush, fatal
// records, shutdown and fork().
struct Core::Impl {
public:
    explicit Impl(DiagnosticHandler handler) : reporter_(std::move(handler)) {
        if (std::optional<std::string> failure = start_dispatcher_()) {
            report_no_dispatcher_(*failure);
        }
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
        if (state_.load(std::memory_order_acquire) != State::Running && !resume_after_fork_()) {
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
    [[nodiscard]] std::expected<void, std::error_code> flush_here();
    [[nodiscard]] std::expected<void, std::error_code> flush_until(Clock::time_point deadline);
    void shut_down() noexcept;
    [[nodiscard]] bool abandoned() const noexcept { return abandoned_; }

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
    void set_shutdown_timeout(std::chrono::milliseconds timeout) noexcept {
        shutdown_timeout_.store(timeout, std::memory_order_relaxed);
    }

    // Keeps an abandoned Impl alive for the rest of the process (see
    // "Shutdown that gives up" in lumen/core.hpp): its dispatch thread may
    // still return from the callback it is stuck in and use it.
    static void keep_abandoned(std::unique_ptr<Impl> impl);

private:
    // Running: normal. ForkChild: in a child of fork(), until the first use
    // starts the dispatch thread again. ShutDown: shut_down() began.
    enum class State : uint8_t {
        Running,
        ForkChild,
        ShutDown
    };

    // The locks the fork handlers hold from prepare to parent or child (see
    // src/fork_handlers.hpp): the short-held ones, never the delivery lock.
    struct ForkLocks {
        std::unique_lock<std::mutex> restart;
        std::unique_lock<std::mutex> dispatcher;
        std::unique_lock<std::mutex> registry;
        std::unique_lock<std::mutex> reporter;
        std::unique_lock<std::mutex> fatals;
        std::unique_lock<std::mutex> wakeup;
        std::unique_lock<std::mutex> flushes;
    };

    template <RecordType R>
    void prepare_(R& record, uint32_t tags_dropped_explicit, SourceStrings source) {
        const uint32_t dropped =
            tags_dropped_explicit + detail::prepare_record(record, source == SourceStrings::Copied);
        if (dropped != 0) {
            tags_dropped_.fetch_add(dropped, std::memory_order_relaxed);
        }
    }

    // Returns why the thread could not be started, if it could not.
    [[nodiscard]] std::optional<std::string> start_dispatcher_();
    void report_no_dispatcher_(std::string_view why);
    void run_dispatcher_();
    void deliver_on_this_thread_();
    // Takes the delivery lock, unless that would close a cycle of waits
    // between Cores (see Core::flush). Waits without a limit otherwise.
    [[nodiscard]] bool begin_session_unless_cycle_(
        std::optional<detail::Delivery::Session>& session);
    [[nodiscard]] bool begin_session_within_limit_(
        std::optional<detail::Delivery::Session>& session);
    [[nodiscard]] bool stop_dispatcher_within_limit_();
    void abandon_shutdown_() noexcept;
    void shut_down_from_delivery_() noexcept;
    void report_first_losses_();

    // Whether the Core is Running after this call: in a fork child, starts
    // the dispatch thread first.
    [[nodiscard]] bool resume_after_fork_();
    void prepare_fork_();
    void after_fork_in_child_() noexcept;

    detail::DiagnosticReporter reporter_;
    detail::FirstReports first_reports_{reporter_};
    detail::RecordQueues queues_;
    detail::Wakeup wakeup_;
    detail::SinkRegistry registry_;
    detail::Delivery delivery_{queues_, registry_, first_reports_};
    detail::FatalRecords fatals_;
    detail::FlushRequests flushes_;
    detail::DispatcherThread dispatcher_;

    std::atomic<State> state_{State::Running};
    std::atomic<uint64_t> tags_dropped_{0};
    std::atomic<std::chrono::milliseconds> shutdown_timeout_{default_shutdown_timeout};
    // Set by the constructor, before any other thread can see this Core, and
    // in a fork child by resume_after_fork_ before state_ becomes Running.
    bool delivers_on_submit_ = false;
    bool abandoned_ = false;               // set by shut_down(), read after it
    std::mutex restart_mutex_;             // serialises resume_after_fork_
    std::optional<ForkLocks> fork_locks_;  // touched only by the thread that forks
    // Last: the handlers use every member above.
    detail::ForkRegistration fork_{detail::ForkHandlers{
        .prepare = [this] { prepare_fork_(); },
        .parent = [this] { fork_locks_.reset(); },
        .child = [this] { after_fork_in_child_(); },
    }};
};

// A Core whose dispatch thread could not be started is still usable: its
// records are delivered by the threads that submit them.
std::optional<std::string> Core::Impl::start_dispatcher_() {
    std::expected<void, std::string> started = dispatcher_.start([this] { run_dispatcher_(); });
    if (started) {
        return std::nullopt;
    }
    delivers_on_submit_ = true;
    return std::move(started.error());
}

void Core::Impl::report_no_dispatcher_(std::string_view why) {
    reporter_.report(
        std::format("lumen: the dispatch thread could not be started ({}); each record is "
                    "delivered on the thread that submits it",
                    why));
}

// Each pass serves the flush requested last, if any (everything queued up to
// the heads read after the request, then every sink's flush()), or else
// delivers what it finds.
void Core::Impl::run_dispatcher_() {
    while (wakeup_.wait_for_work([this] { return queues_.has_pending() || flushes_.pending(); })) {
        const uint64_t flush_ticket = flushes_.latest();
        uint64_t delivered = 0;
        {
            const detail::Delivery::Session session(delivery_);
            if (flush_ticket != 0) {
                delivery_.deliver_all_up_to(queues_.heads());
                delivery_.flush_sinks();
            } else {
                delivered = delivery_.deliver_up_to(queues_.heads());
            }
        }
        report_first_losses_();
        if (flush_ticket != 0) {
            flushes_.complete(flush_ticket);
        } else if (delivered == 0) {
            // A claimed record that is not yet published cannot be popped;
            // let its producer run instead of spinning on it.
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

bool Core::Impl::begin_session_unless_cycle_(std::optional<detail::Delivery::Session>& session) {
    const detail::Delivery::Waiting waiting(delivery_);
    if (!waiting.can_be_in_cycle()) {
        session.emplace(delivery_);
        return true;
    }
    while (true) {
        session.emplace(delivery_, detail::FlushRequests::cycle_check_interval);
        if (session->owns()) {
            return true;
        }
        session.reset();
        if (waiting.in_cycle()) {
            return false;
        }
    }
}

std::expected<void, std::error_code> Core::Impl::flush_here() {
    if (delivery_.delivering_here()) {
        return std::unexpected(make_error_code(LumenError::SinkBusy));
    }
    if (state_.load(std::memory_order_acquire) != State::Running && !resume_after_fork_()) {
        return std::unexpected(make_error_code(LumenError::CoreShutDown));
    }
    const detail::QueuePositions targets = queues_.heads();
    {
        std::optional<detail::Delivery::Session> session;
        if (!begin_session_unless_cycle_(session)) {
            return std::unexpected(make_error_code(LumenError::SinkBusy));
        }
        delivery_.deliver_all_up_to(targets);
        delivery_.flush_sinks();
    }
    report_first_losses_();
    return {};
}

std::expected<void, std::error_code> Core::Impl::flush_until(Clock::time_point deadline) {
    if (delivers_on_submit_) {
        return flush_here();
    }
    if (delivery_.delivering_here()) {
        return std::unexpected(make_error_code(LumenError::SinkBusy));
    }
    if (state_.load(std::memory_order_acquire) != State::Running && !resume_after_fork_()) {
        return std::unexpected(make_error_code(LumenError::CoreShutDown));
    }
    const uint64_t ticket = flushes_.request();
    wakeup_.wake_if_sleeping();
    const detail::Delivery::Waiting waiting(delivery_);
    switch (flushes_.wait(ticket, deadline, waiting.can_be_in_cycle(),
                          [&waiting] { return waiting.in_cycle(); })) {
        case detail::FlushRequests::Outcome::Done: return {};
        case detail::FlushRequests::Outcome::TimedOut:
            return std::unexpected(make_error_code(LumenError::FlushTimedOut));
        case detail::FlushRequests::Outcome::Closed:
            return std::unexpected(make_error_code(LumenError::CoreShutDown));
        case detail::FlushRequests::Outcome::Cycle:
            return std::unexpected(make_error_code(LumenError::SinkBusy));
    }
    return std::unexpected(make_error_code(LumenError::CoreShutDown));  // not reached
}

// Waits for the delivery lock while whoever holds it makes progress.
bool Core::Impl::begin_session_within_limit_(std::optional<detail::Delivery::Session>& session) {
    StallWatch watch(shutdown_timeout_.load(std::memory_order_relaxed), delivery_);
    while (true) {
        session.emplace(delivery_, stall_poll_interval);
        if (session->owns()) {
            return true;
        }
        session.reset();
        if (watch.stalled()) {
            return false;
        }
    }
}

void Core::Impl::submit_fatal(LogRecord& record, uint32_t tags_dropped_explicit,
                              SourceStrings source) {
    if (state_.load(std::memory_order_acquire) == State::ShutDown) {
        queues_.count_after_shutdown<LogRecord>();
        detail::terminate_after_fatal();
    }
    fatals_.announce();
    prepare_(record, tags_dropped_explicit, source);
    fatals_.post(std::move(record));
    if (delivery_.delivering_here()) {
        // Inside a callback: this thread already holds a Session.
        fatals_.deliver_all(delivery_);
        report_first_losses_();
        detail::terminate_after_fatal();
    }
    {
        std::optional<detail::Delivery::Session> session;
        if (begin_session_within_limit_(session)) {
            fatals_.deliver_all(delivery_);
        } else {
            reporter_.report(std::format(
                "lumen: a fatal record was not delivered: the thread delivering records made no "
                "progress for {} ms (a sink callback that does not return); the process ends now",
                shutdown_timeout_.load(std::memory_order_relaxed).count()));
        }
    }
    report_first_losses_();
    detail::terminate_after_fatal();
}

void Core::Impl::shut_down() noexcept {
    if (state_.exchange(State::ShutDown, std::memory_order_acq_rel) == State::ShutDown) {
        return;
    }
    registry_.close();
    wakeup_.stop();
    if (delivery_.delivering_here()) {
        shut_down_from_delivery_();
        return;
    }
    std::optional<detail::Delivery::Session> session;
    if (!stop_dispatcher_within_limit_() || !begin_session_within_limit_(session)) {
        abandon_shutdown_();
        return;
    }
    delivery_.deliver_everything_queued();
    delivery_.flush_sinks();
    delivery_.release_snapshot();
    session.reset();
    flushes_.complete_all();
    flushes_.close();
    report_first_losses_();
    registry_.destroy_sinks();
}

// Returns false if the dispatch thread stopped making progress before it
// returned (see "Shutdown that gives up" in lumen/core.hpp).
bool Core::Impl::stop_dispatcher_within_limit_() {
    if (!dispatcher_.running()) {
        return true;
    }
    StallWatch watch(shutdown_timeout_.load(std::memory_order_relaxed), delivery_);
    while (!dispatcher_.wait_for_exit(stall_poll_interval)) {
        if (watch.stalled()) {
            return false;
        }
    }
    dispatcher_.join();
    return true;
}

// Neither the queued records nor the sinks can be touched: the thread that
// is stuck holds the delivery lock (or the dispatch thread waits for it).
// Everything stays as it is, for that thread to find if it ever returns.
void Core::Impl::abandon_shutdown_() noexcept {
    abandoned_ = true;
    dispatcher_.detach();
    flushes_.close();
    reporter_.report(std::format(
        "lumen: shutdown gave up after {} ms without progress: a sink callback (or the "
        "diagnostic handler) did not return; {} queued record(s) were not delivered, and the "
        "sinks were neither flushed nor destroyed",
        shutdown_timeout_.load(std::memory_order_relaxed).count(), queues_.queued()));
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
    flushes_.complete_all();
    flushes_.close();
    report_first_losses_();
    dispatcher_.detach();
}

void Core::Impl::report_first_losses_() {
    first_reports_.report_new_losses(queues_.refused_when_full(),
                                     tags_dropped_.load(std::memory_order_relaxed));
}

bool Core::Impl::resume_after_fork_() {
    std::optional<std::string> failure;
    {
        const std::scoped_lock lock(restart_mutex_);
        if (state_.load(std::memory_order_acquire) == State::ForkChild) {
            failure = start_dispatcher_();
            state_.store(State::Running, std::memory_order_release);
        }
    }
    if (failure) {
        report_no_dispatcher_(*failure);
    }
    return state_.load(std::memory_order_acquire) == State::Running;
}

void Core::Impl::prepare_fork_() {
    fork_locks_.emplace(ForkLocks{
        .restart = std::unique_lock(restart_mutex_),
        .dispatcher = dispatcher_.lock_for_fork(),
        .registry = registry_.lock_for_fork(),
        .reporter = reporter_.lock_for_fork(),
        .fatals = fatals_.lock_for_fork(),
        .wakeup = wakeup_.lock_for_fork(),
        .flushes = flushes_.lock_for_fork(),
    });
}

// The child has only the thread that forked. If that thread was inside a
// callback of this Core, it goes on with that delivery (and, if it is the
// dispatch thread, stays the child's): the delivery lock, the sinks' busy
// flags and the dispatcher are left as they are. Otherwise the delivery lock
// may be held by a thread of the parent and is re-initialised, and the
// dispatch thread is started again at the first use.
void Core::Impl::after_fork_in_child_() noexcept {
    queues_.discard_in_fork_child();
    const bool forked_in_callback = delivery_.delivering_here();
    if (!forked_in_callback) {
        delivery_.reset_in_fork_child();
        registry_.clear_busy_in_fork_child();
        dispatcher_.reset_in_fork_child();
    }
    wakeup_.reset_in_fork_child();
    flushes_.reset_in_fork_child();
    fatals_.reset_in_fork_child();
    reporter_.reset_in_fork_child();
    fork_locks_.reset();
    State running = State::Running;
    if (!forked_in_callback && !delivers_on_submit_) {
        static_cast<void>(
            state_.compare_exchange_strong(running, State::ForkChild, std::memory_order_acq_rel));
    }
}

CoreStatistics Core::Impl::statistics() const noexcept {
    QueueStatistics logs = queues_.statistics<LogRecord>();
    logs.accepted += fatals_.announced();
    return CoreStatistics{logs, queues_.statistics<MetricRecord>(),
                          queues_.statistics<ProgressRecord>(),
                          tags_dropped_.load(std::memory_order_relaxed)};
}

void Core::Impl::keep_abandoned(std::unique_ptr<Impl> impl) {
    struct Abandoned {
        std::mutex mutex;
        std::vector<std::unique_ptr<Impl>> impls;  // guarded by mutex
    };
    static detail::Immortal<Abandoned> abandoned;
    const std::scoped_lock lock(abandoned.get().mutex);
    abandoned.get().impls.push_back(std::move(impl));
}

Core::Core(DiagnosticHandler diagnostics) : impl_(std::make_unique<Impl>(std::move(diagnostics))) {}

// Shuts down before impl_ starts to be destroyed, so a sink destructor that
// calls back into this Core still finds impl_ (std::unique_ptr may clear its
// pointer before it deletes).
Core::~Core() {
    impl_->shut_down();
    if (impl_->abandoned()) {
        Impl::keep_abandoned(std::move(impl_));
    }
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
    // flush() is documented to return without delivering in the cases the
    // error names (inside its own callback, in a cycle, after shutdown);
    // flush_for() is the call that reports them.
    [[maybe_unused]] const std::expected<void, std::error_code> flushed = impl_->flush_here();
}

std::expected<void, std::error_code> Core::flush_for(std::chrono::milliseconds timeout) {
    return impl_->flush_until(deadline_after(timeout));
}

CoreStatistics Core::statistics() const noexcept {
    return impl_->statistics();
}

void Core::set_diagnostic_handler(DiagnosticHandler handler) {
    impl_->set_diagnostic_handler(std::move(handler));
}

void Core::set_shutdown_timeout(std::chrono::milliseconds timeout) noexcept {
    impl_->set_shutdown_timeout(timeout);
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
