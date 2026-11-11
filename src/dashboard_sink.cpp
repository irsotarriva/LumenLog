#include "lumen/dashboard_sink.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "dashboard_frame.hpp"
#include "lumen/error.hpp"
#include "lumen/thread_clock.hpp"
#include "record_format.hpp"
#include "terminal_output.hpp"
#include "time_format.hpp"

namespace lumen {

namespace {

constexpr std::string_view enter_screen = "\x1b[?1049h\x1b[?25l";  // alternate screen, hide cursor
constexpr std::string_view leave_screen = "\x1b[?25h\x1b[?1049l";  // show cursor, normal screen
constexpr std::string_view cursor_home = "\x1b[H";

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
        , refresh_interval(interval)
        , diagnostics(std::move(diagnostic_handler))
        , model(std::move(initial_model)) {}

    ~Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    void start() {
        write_(enter_screen);
        thread = std::thread([this] { run_(); });
    }

    // Stops drawing; returns once the thread is gone. Idempotent.
    void stop() noexcept {
        {
            const std::scoped_lock lock(mutex);
            stopping = true;
        }
        wake_cv.notify_one();
        if (thread.joinable()) {
            thread.join();
        }
    }

    // Called on the sink's callback thread with the drawing thread stopped.
    void restore_terminal() {
        if (restored) {
            return;
        }
        restored = true;
        write_(leave_screen);
    }

    // After a Fatal record: the terminal back to normal, then the log lines
    // as plain text, so that the fatal message outlives the process.
    void restore_after_fatal() {
        if (restored) {
            return;  // a second flush() after the fatal record
        }
        stop();
        restore_terminal();
        std::string text;
        {
            const std::scoped_lock lock(mutex);
            for (const DashboardModel::LogLine& line : model.scrollback()) {
                text += line.text;
                text += '\n';
            }
        }
        write_(text);
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
    const DiagnosticHandler diagnostics;  // used by the drawing thread, or after it stopped
    std::atomic<bool> failure_reported{false};
    std::atomic<uint64_t> frames_written{0};
    std::atomic<uint64_t> frames_failed{0};
    bool restored = false;  // touched only with the drawing thread stopped

    std::mutex mutex;
    std::condition_variable wake_cv;   // waits on mutex: change, frame request or stop
    std::condition_variable frame_cv;  // waits on mutex: frames_done advanced
    DashboardModel model;              // guarded by mutex
    bool dirty = false;                // guarded by mutex: model changed since the last frame
    bool fatal_seen = false;           // guarded by mutex
    bool stopping = false;             // guarded by mutex
    uint64_t frames_requested = 0;     // guarded by mutex
    uint64_t frames_done = 0;          // guarded by mutex: requests satisfied so far

    // std::thread rather than std::jthread: Apple's libc++ has no jthread.
    std::thread thread;

private:
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

    void draw_(const DashboardModel& snapshot) noexcept {
        std::string text;
        try {
            text = std::string(cursor_home) + detail::render_frame(snapshot, frame);
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
    bool write_(std::string_view text) noexcept {
        try {
            std::ostream& stream = output.stream();
            stream.write(text.data(), static_cast<std::streamsize>(text.size()));
            stream.flush();
            if (stream.good()) {
                return true;
            }
            stream.clear();
            frame_failed_("the stream reported an error");
        } catch (...) {
            frame_failed_("the stream threw an exception");
        }
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

std::expected<std::unique_ptr<DashboardSink>, std::error_code> DashboardSink::create(
    Config config) {
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
    impl->start();
    return std::make_unique<DashboardSink>(ConstructionKey{}, std::move(impl));
}

DashboardSink::DashboardSink(ConstructionKey /*key*/, std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

DashboardSink::~DashboardSink() {
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
        impl_->restore_after_fatal();
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

}  // namespace lumen
