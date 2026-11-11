#include "lumen/dashboard_sink.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <format>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef PLATFORM_WINDOWS
#include <io.h>
#else
#include <unistd.h>
#endif

#include "dashboard_frame.hpp"
#include "fork_handlers.hpp"
#include "lumen/detail/immortal.hpp"
#include "lumen/error.hpp"
#include "lumen/thread_clock.hpp"
#include "portable_error.hpp"
#include "record_format.hpp"
#include "terminal_output.hpp"
#include "termination.hpp"
#include "time_format.hpp"

namespace lumen {

namespace {

constexpr std::string_view enter_screen = "\x1b[?1049h\x1b[?25l";  // alternate screen, hide cursor
constexpr std::string_view leave_screen = "\x1b[?25h\x1b[?1049l";  // show cursor, normal screen
constexpr std::string_view cursor_home = "\x1b[H";

// Larger intervals are taken as this one, so that "now + interval" cannot
// overflow the clock.
constexpr std::chrono::milliseconds max_refresh_interval = std::chrono::hours{24};
// On the ways out of the process, how long to wait for the drawing thread to
// finish its frame.
constexpr std::chrono::seconds exit_wait{1};

constexpr uint16_t fallback_width = 80;
constexpr uint16_t fallback_height = 24;

// Live dashboards that draw to standard error and have not restored it.
// A namespace-scope atomic because restore_terminal_after_signal() runs in a
// signal handler, which can safely reach nothing else; constant-initialised,
// so it exists before and after every other static.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): see above
std::atomic<uint32_t> dashboards_on_standard_error{0};
static_assert(std::atomic<uint32_t>::is_always_lock_free);

// A log line as the dashboard shows it: the text format without colour and
// without the line's final '\n'.
[[nodiscard]] std::string scroll_text(const LogRecord& record, const detail::TextStyle& style) {
    std::string line = detail::text_line(record, style);
    if (!line.empty() && line.back() == '\n') {
        line.pop_back();
    }
    return line;
}

}  // namespace

struct DashboardSink::Impl {
    Impl(detail::OutputStream output_stream, DashboardModel initial_model,
         detail::TextStyle text_style, detail::FrameSettings frame_settings,
         std::chrono::milliseconds interval, DiagnosticHandler diagnostic_handler)
        : output(std::move(output_stream))
        , style(std::move(text_style))
        , frame(frame_settings)
        , refresh_interval(std::min(interval, max_refresh_interval))
        , diagnostics(std::move(diagnostic_handler))
        , model(std::move(initial_model)) {}

    ~Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    // Switches to the alternate screen and starts the drawing thread. If the
    // thread cannot start, the screen is switched back.
    [[nodiscard]] std::expected<void, std::error_code> start() noexcept {
        static_cast<void>(write_(enter_screen));  // a failure is counted in frames_failed
        if (output.is_standard_error()) {
            dashboards_on_standard_error.fetch_add(1, std::memory_order_relaxed);
        }
        std::expected<void, std::error_code> started = start_();
        if (!started) {
            restore_terminal();
        }
        return started;
    }

    // Stops drawing; returns once the thread is gone. Idempotent.
    void stop() noexcept {
        request_stop_();
        if (thread.joinable() && thread.get_id() != std::this_thread::get_id()) {
            thread.join();
        }
    }

    // Restores the terminal once, whichever path gets here first. Called
    // with the drawing thread stopped, or on a way out of the process.
    void restore_terminal() noexcept {
        if (restored.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        static_cast<void>(write_(leave_screen));  // a failure is counted in frames_failed
        if (output.is_standard_error()) {
            dashboards_on_standard_error.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    // The process is ending without the destructor (a fatal record, a
    // terminate handler, an exit function): stops drawing, waiting at most
    // exit_wait for the frame being drawn, restores the terminal and, if
    // `with_log_lines`, writes the log lines as plain text so that they
    // outlive the process. Idempotent.
    void restore_on_the_way_out(bool with_log_lines) noexcept {
        stop_within_(exit_wait);
        if (restored.load(std::memory_order_acquire)) {
            return;
        }
        restore_terminal();
        if (with_log_lines) {
            static_cast<void>(write_(log_lines_()));
        }
    }

    // Marks the model changed and wakes the drawing thread if it was idle.
    void changed_locked() {
        if (!dirty) {
            dirty = true;
            wake_cv.notify_one();
        }
    }

    // Asks for a frame and waits until one drawn after this call is written.
    void draw_now() {
        std::unique_lock lock(mutex);
        if (stopping) {
            return;
        }
        const uint64_t ticket = ++frames_requested;
        wake_cv.notify_one();
        frame_cv.wait(lock, [&] { return frames_done >= ticket || stopping; });
    }

    const detail::OutputStream output;
    const detail::TextStyle style;
    const detail::FrameSettings frame;
    const std::chrono::milliseconds refresh_interval;
    // Called by one thread at a time: the drawing thread, or a thread that
    // writes after stopping it.
    const DiagnosticHandler diagnostics;
    std::atomic<bool> failure_reported{false};
    std::atomic<uint64_t> frames_written{0};
    std::atomic<uint64_t> frames_failed{0};
    std::atomic<bool> restored{false};

    std::mutex mutex;
    std::condition_variable wake_cv;   // waits on mutex: change, frame request or stop
    std::condition_variable frame_cv;  // waits on mutex: frames_done advanced, or the thread ended
    DashboardModel model;              // guarded by mutex
    bool dirty = false;                // guarded by mutex: model changed since the last frame
    bool fatal_seen = false;           // guarded by mutex
    bool stopping = false;             // guarded by mutex
    bool finished = false;             // guarded by mutex: the drawing thread has returned
    uint64_t frames_requested = 0;     // guarded by mutex
    uint64_t frames_done = 0;          // guarded by mutex: requests satisfied so far

    // std::thread rather than std::jthread: Apple's libc++ has no jthread.
    std::thread thread;

    // Held by the fork handlers from prepare to parent or child; touched
    // only by the thread that forks.
    std::optional<std::unique_lock<std::mutex>> fork_lock;
    // Last: its handlers use the members above.
    const detail::ForkRegistration fork{detail::ForkHandlers{
        .prepare = [this] { fork_lock.emplace(mutex); },
        .parent = [this] { fork_lock.reset(); },
        .child = [this] { after_fork_in_child_(); },
    }};

private:
    // In a fork child the terminal belongs to the parent and the drawing
    // thread does not exist: the sink stops drawing for good, as if stopped
    // and restored, without writing anything. Runs with `mutex` held by this
    // thread (the prepare handler); the condition variables and the thread
    // handle belong to the parent's threads and are re-initialised.
    void after_fork_in_child_() noexcept {
        std::construct_at(&wake_cv);
        std::construct_at(&frame_cv);
        std::construct_at(&thread);
        stopping = true;
        finished = true;
        frames_done = frames_requested;
        if (!restored.exchange(true, std::memory_order_acq_rel) && output.is_standard_error()) {
            dashboards_on_standard_error.fetch_sub(1, std::memory_order_relaxed);
        }
        fork_lock.reset();
    }

    // Starts the drawing thread; start() calls it once.
    [[nodiscard]] std::expected<void, std::error_code> start_() noexcept {
        try {
            thread = std::thread([this] { run_(); });
        } catch (const std::system_error& e) {
            return std::unexpected(detail::portable_error(e.code()));
        } catch (const std::bad_alloc&) {
            return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
        }
        return {};
    }

    void request_stop_() noexcept {
        {
            const std::scoped_lock lock(mutex);
            stopping = true;
        }
        wake_cv.notify_one();
    }

    // As stop(), but gives up after `timeout`: a terminal that blocks must
    // not keep a terminating process alive. The thread is then left running
    // and not joined, which is harmless as the process ends.
    void stop_within_(std::chrono::milliseconds timeout) noexcept {
        if (!thread.joinable() || thread.get_id() == std::this_thread::get_id()) {
            return;  // never started, already joined, or this is the drawing thread
        }
        std::unique_lock lock(mutex);
        stopping = true;
        wake_cv.notify_one();
        if (frame_cv.wait_for(lock, timeout, [&] { return finished; })) {
            lock.unlock();
            thread.join();
        }
    }

    // The log lines, oldest first, one per line.
    [[nodiscard]] std::string log_lines_() noexcept {
        try {
            std::string text;
            const std::scoped_lock lock(mutex);
            for (const DashboardModel::LogLine& line : model.scrollback()) {
                text += line.text;
                text += '\n';
            }
            return text;
        } catch (...) {
            return {};  // no memory for the copy; the terminal is restored anyway
        }
    }

    void run_() noexcept {
        std::unique_lock lock(mutex);
        auto next_frame = std::chrono::steady_clock::now() + refresh_interval;
        while (true) {
            // Wait for a frame request, or for something to draw (a change, or
            // bars that may expire) once the refresh interval has passed.
            wake_cv.wait(lock, [&] {
                return stopping || frames_requested > frames_done || dirty ||
                       !model.progress_bars().empty();
            });
            wake_cv.wait_until(lock, next_frame,
                               [&] { return stopping || frames_requested > frames_done; });
            if (stopping) {
                frames_done = frames_requested;
                finished = true;
                frame_cv.notify_all();
                return;
            }
            const std::size_t bars_before = model.progress_bars().size();
            model.expire_progress(now_ns());
            const bool requested = frames_requested > frames_done;
            if (!dirty && !requested && model.progress_bars().size() == bars_before) {
                next_frame = std::chrono::steady_clock::now() + refresh_interval;
                continue;
            }
            const uint64_t ticket = frames_requested;
            const DashboardModel snapshot = model;
            dirty = false;
            lock.unlock();
            draw_(snapshot);
            lock.lock();
            frames_done = ticket;
            frame_cv.notify_all();
            next_frame = std::chrono::steady_clock::now() + refresh_interval;
        }
    }

    // The configured size, with each dimension left at 0 measured on the
    // destination now (the terminal may have been resized).
    [[nodiscard]] detail::FrameSettings frame_settings_() const noexcept {
        detail::FrameSettings settings = frame;
        if (settings.width > 0 && settings.height > 0) {
            return settings;
        }
        const std::optional<detail::TerminalSize> measured = output.terminal_size();
        if (settings.width == 0) {
            settings.width = measured ? measured->columns : fallback_width;
        }
        if (settings.height == 0) {
            settings.height = measured ? measured->rows : fallback_height;
        }
        return settings;
    }

    void draw_(const DashboardModel& snapshot) noexcept {
        std::string text;
        try {
            text = std::string(cursor_home) + detail::render_frame(snapshot, frame_settings_());
        } catch (const std::exception& e) {
            frame_failed_(std::string("drawing failed: ") + e.what());
            return;
        } catch (...) {
            frame_failed_("drawing failed");
            return;
        }
        if (write_(text)) {
            frames_written.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Writes and flushes; a failure counts one failed frame.
    [[nodiscard]] bool write_(std::string_view text) noexcept {
        const std::array<std::string_view, 1> pieces = {text};
        const std::optional<std::string> failure = detail::write_text(output.stream(), pieces);
        if (!failure) {
            return true;
        }
        frame_failed_(*failure);
        return false;
    }

    void frame_failed_(std::string_view why) noexcept {
        frames_failed.fetch_add(1, std::memory_order_relaxed);
        if (failure_reported.exchange(true, std::memory_order_relaxed) || !diagnostics) {
            return;
        }
        try {
            diagnostics(
                std::format("lumen: DashboardSink: frame lost ({}); later failures are "
                            "only counted (frames_failed())",
                            why));
        } catch (...) {
            return;  // nowhere left to report to; the failure is counted
        }
    }
};

namespace {

// The dashboards alive in the process, for the ways out on which no
// destructor of theirs runs (see "The terminal" in lumen/dashboard_sink.hpp).
// While the list is not empty, Lumen's terminate handler is installed.
//
// Lock order: the list's mutex before a dashboard's (restore_all_). The fork
// handlers take them in registration order, so the list registers before
// the first dashboard (create() asks for the instance first).
class LiveDashboards {
public:
    // Never destroyed: an exit function or the terminate handler may run
    // during static destruction.
    [[nodiscard]] static LiveDashboards& instance() {
        static detail::Immortal<LiveDashboards> live;
        return live.get();
    }

    // May throw std::bad_alloc.
    void add(DashboardSink::Impl& impl) {
        const std::scoped_lock lock(mutex_);
        live_.push_back(&impl);
        install_handlers_locked_();
    }

    void remove(DashboardSink::Impl& impl) noexcept {
        const std::scoped_lock lock(mutex_);
        std::erase(live_, &impl);
        if (live_.empty() && std::get_terminate() == &restore_then_terminate_) {
            std::set_terminate(previous_terminate_.load(std::memory_order_relaxed));
        }
    }

private:
    // Lumen's terminate handler: restores every live dashboard, then hands
    // over to the handler installed before it (src/termination.hpp).
    [[noreturn]] static void restore_then_terminate_() noexcept {
        LiveDashboards& live = instance();
        live.restore_all_(true);
        detail::hand_over_terminate(live.previous_terminate_.load(std::memory_order_relaxed));
    }

    static void restore_at_exit_() noexcept { instance().restore_all_(false); }

    void install_handlers_locked_() {
        if (std::get_terminate() != &restore_then_terminate_) {
            previous_terminate_.store(std::set_terminate(&restore_then_terminate_),
                                      std::memory_order_relaxed);
        }
        if (exit_functions_registered_) {
            return;
        }
        exit_functions_registered_ = true;
        // A registration that fails (the C library's table is full) leaves
        // that one way out unrestored; the dashboard itself works on.
        static_cast<void>(std::atexit(&restore_at_exit_));
#ifndef PLATFORM_MACOS
        static_cast<void>(std::at_quick_exit(&restore_at_exit_));
#endif
    }

    // add() and remove() hold the list only briefly and call nothing that
    // could terminate meanwhile, so waiting for it here cannot deadlock.
    void restore_all_(bool with_log_lines) noexcept {
        const std::scoped_lock lock(mutex_);
        for (DashboardSink::Impl* const impl : live_) {
            impl->restore_on_the_way_out(with_log_lines);
        }
    }

    std::mutex mutex_;
    // Guarded by mutex_. Non-owning: each dashboard removes itself in its
    // destructor, before its Impl is destroyed.
    std::vector<DashboardSink::Impl*> live_;
    bool exit_functions_registered_ = false;  // guarded by mutex_
    // Written under mutex_; read by the terminate handler without it.
    std::atomic<std::terminate_handler> previous_terminate_{nullptr};
    // mutex_, held by the fork handlers from prepare to parent or child;
    // touched only by the thread that forks.
    std::optional<std::unique_lock<std::mutex>> fork_lock_;
    // Last: its handlers use the members above.
    const detail::ForkRegistration fork_{detail::ForkHandlers{
        .prepare = [this] { fork_lock_.emplace(mutex_); },
        .parent = [this] { fork_lock_.reset(); },
        .child = [this] { fork_lock_.reset(); },
    }};
};

}  // namespace

std::expected<std::unique_ptr<DashboardSink>, std::error_code> DashboardSink::create(
    Config config) {
    LiveDashboards& live = LiveDashboards::instance();  // first: see its lock order
    if (config.refresh_interval < std::chrono::milliseconds{1}) {
        return std::unexpected(make_error_code(LumenError::InvalidSinkConfig));
    }
    std::expected<detail::TimeFormat, std::error_code> time =
        detail::TimeFormat::parse(config.time_format, config.time_zone);
    if (!time) {
        return std::unexpected(time.error());
    }
    detail::OutputStream output(std::move(config.output));
    if (!output.is_terminal() && !config.assume_terminal) {
        return std::unexpected(make_error_code(LumenError::NotATerminal));
    }
    detail::TextStyle style{
        .time = std::move(*time),
        .colors = std::nullopt,
        .shown_tags = std::move(config.inline_tags),
        .show_thread = false,
        .show_source = false,
    };
    const detail::FrameSettings frame{.width = config.width,
                                      .height = config.height,
                                      .panel_rows = config.panel_rows,
                                      .colors = config.colors};
    auto impl = std::make_unique<Impl>(
        std::move(output), DashboardModel(config.limits, std::move(config.metrics)),
        std::move(style), frame, config.refresh_interval, std::move(config.diagnostics));
    try {
        live.add(*impl);
    } catch (const std::bad_alloc&) {
        return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
    }
    if (const std::expected<void, std::error_code> started = impl->start(); !started) {
        live.remove(*impl);
        return std::unexpected(started.error());
    }
    return std::make_unique<DashboardSink>(ConstructionKey{}, std::move(impl));
}

DashboardSink::DashboardSink(ConstructionKey /*key*/, std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

DashboardSink::~DashboardSink() {
    LiveDashboards::instance().remove(*impl_);
    impl_->stop();
    impl_->restore_terminal();
}

void DashboardSink::on_log(const LogRecord& record) {
    std::string text = scroll_text(record, impl_->style);
    const std::scoped_lock lock(impl_->mutex);
    impl_->model.add_log(record.level, std::move(text));
    if (record.level == LogLevel::Fatal) {
        impl_->fatal_seen = true;
    }
    impl_->changed_locked();
}

void DashboardSink::on_metric(const MetricRecord& record) {
    const std::scoped_lock lock(impl_->mutex);
    impl_->model.add_metric(record.name, record.value);
    impl_->changed_locked();
}

void DashboardSink::on_progress(const ProgressRecord& record) {
    const std::scoped_lock lock(impl_->mutex);
    impl_->model.add_progress(record);
    impl_->changed_locked();
}

void DashboardSink::flush() {
    bool fatal = false;
    {
        const std::scoped_lock lock(impl_->mutex);
        fatal = impl_->fatal_seen;
    }
    if (fatal) {
        // Core terminates the process next.
        impl_->restore_on_the_way_out(true);
        return;
    }
    impl_->draw_now();
}

uint64_t DashboardSink::frames_written() const noexcept {
    return impl_->frames_written.load(std::memory_order_relaxed);
}

uint64_t DashboardSink::frames_failed() const noexcept {
    return impl_->frames_failed.load(std::memory_order_relaxed);
}

void DashboardSink::restore_terminal_after_signal() noexcept {
    if (dashboards_on_standard_error.load(std::memory_order_relaxed) == 0) {
        return;
    }
#ifdef PLATFORM_WINDOWS
    const auto written = _write(2, leave_screen.data(), static_cast<unsigned>(leave_screen.size()));
#else
    const auto written = ::write(STDERR_FILENO, leave_screen.data(), leave_screen.size());
#endif
    // Nothing can be done in a signal handler about a failed write. (A cast
    // of the call itself to void does not satisfy glibc's warn_unused_result.)
    static_cast<void>(written);
}

}  // namespace lumen
