#ifndef LUMEN_DASHBOARD_SINK_HPP
#define LUMEN_DASHBOARD_SINK_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <ostream>
#include <string>
#include <system_error>
#include <vector>

#include "lumen/dashboard_model.hpp"
#include "lumen/diagnostics.hpp"
#include "lumen/sink.hpp"
#include "lumen/terminal_style.hpp"

// Only in a library built with LUMEN_ENABLE_DASHBOARD=ON (it needs FTXUI);
// otherwise this header declares a class that has no definition.

namespace lumen {

// A full-screen live view on a terminal: a panel with the chosen metrics
// (latest value, range and a sparkline of the recent history) and the
// progress bars in flight, above the most recent log lines in their level's
// colour. What is kept, and when bars disappear, is DashboardModel's
// business (lumen/dashboard_model.hpp).
//
// Threads. on_* only updates the model under a mutex that is otherwise held
// just long enough to copy the model; it never draws or writes. A thread
// owned by the sink draws a frame at most once per refresh_interval, and only
// when something changed (or a progress bar may have expired). flush() asks
// for a frame and returns when it has been written.
//
// Frame size. Config::width and height when both are set; otherwise each
// frame measures the terminal on standard error (the destination's own
// descriptor), and a dimension that cannot be measured (a caller's stream,
// or standard error not a terminal) is 80 columns or 24 rows.
//
// The terminal. create() switches the terminal to its alternate screen and
// hides the cursor. Every way out that Lumen can see restores it (shows the
// cursor and switches back, which brings back what was on the screen):
//   - the destructor;
//   - a fatal record (LUMEN_LOG_FATAL), which Core follows with flush() and
//     std::terminate, so that no destructor runs. If the fatal record reached
//     this sink, flush() stops drawing, restores the terminal and writes the
//     log lines it holds (the fatal one last) as plain text to the normal
//     screen, where they stay after the process ends. If the sink's
//     predicate filtered the record out, the terminate handler below does
//     the same;
//   - std::terminate for any other reason (an uncaught exception): while a
//     DashboardSink exists, Lumen's terminate handler restores the terminal
//     of every live dashboard, writes their log lines as above, and then
//     calls the handler that was installed before it (by default the one that
//     aborts). An application that calls std::set_terminate after creating a
//     dashboard replaces Lumen's handler and with it this step;
//   - std::exit, and a return from main, while the sink is still alive (in a
//     Core that is never destroyed, or one whose destructor does not run):
//     a function registered with std::atexit restores the terminal of every
//     live dashboard; std::quick_exit likewise through std::at_quick_exit,
//     except on macOS, whose C library has no at_quick_exit.
// Each of these waits for the drawing thread to finish its frame, at most
// about one second.
//
// Signals are not handled: installing signal handlers is the application's
// decision, and Lumen installs none. A signal whose default action ends the
// process (SIGINT from Ctrl-C, SIGTERM, SIGQUIT, SIGSEGV, SIGABRT from
// abort()) leaves the terminal in the alternate screen with the cursor
// hidden; `reset` or `tput rmcup; tput cnorm` in the shell repairs it. To
// avoid that, install a handler for the signals you care about and call
// DashboardSink::restore_terminal_after_signal() from it, then end the
// process (for instance by restoring the default action and raising the
// signal again); see docs/sinks.md for an example.
class DashboardSink final : public Sink {
public:
    struct Config {
        // Null: the process's standard error. Must be a terminal (see
        // create()); the sink is the only writer while it exists.
        std::shared_ptr<std::ostream> output;
        // Treat `output` as a terminal although it cannot be recognised as
        // one (a caller's stream over a pseudo-terminal, or a test).
        bool assume_terminal = false;
        // As DashboardModel's constructor.
        std::vector<std::string> metrics;
        DashboardModel::Limits limits;
        // Smallest time between two frames; at least 1 ms. A value above
        // 24 hours is taken as 24 hours.
        std::chrono::milliseconds refresh_interval{100};
        // Frame size in cells; 0: the terminal's size (see "Frame size").
        uint16_t width = 0;
        uint16_t height = 0;
        // Most rows the metric and progress panel may take.
        std::size_t panel_rows = 8;
        LevelColors colors = default_level_colors;
        // The time column of the log lines; as TerminalSink::Config.
        std::string time_format = "%H:%M:%S";
        TimeZone time_zone = TimeZone::Local;
        std::vector<std::string> inline_tags;
        // Receives one line about the first failed draw or write; later
        // failures are only counted (frames_failed()). Called on the drawing
        // thread, or on the thread that destroys or flushes the sink or ends
        // the process, never concurrently with itself.
        DiagnosticHandler diagnostics = stderr_diagnostics();
    };

    // Fails with LumenError::NotATerminal if the destination is not a
    // terminal and assume_terminal is false, with
    // LumenError::InvalidSinkConfig for an invalid time_format or a
    // refresh_interval below 1 ms, and with the error std::thread reports
    // (std::errc::resource_unavailable_try_again when the system is out of
    // threads) if the drawing thread cannot be started; the terminal is then
    // left as it was.
    [[nodiscard]] static std::expected<std::unique_ptr<DashboardSink>, std::error_code> create(
        Config config);

    // Only create() can make the key, so only create() can construct a sink.
    class ConstructionKey {
        friend class DashboardSink;
        ConstructionKey() = default;
    };
    struct Impl;
    DashboardSink(ConstructionKey key, std::unique_ptr<Impl> impl);
    ~DashboardSink() override;
    DashboardSink(const DashboardSink&) = delete;
    DashboardSink& operator=(const DashboardSink&) = delete;
    DashboardSink(DashboardSink&&) = delete;
    DashboardSink& operator=(DashboardSink&&) = delete;

    void on_log(const LogRecord& record) override;
    void on_metric(const MetricRecord& record) override;
    void on_progress(const ProgressRecord& record) override;
    void flush() override;

    // Frames written without an error, and writes lost to a failed draw or a
    // stream error (frames, and the escapes that switch screens). Safe to
    // call from any thread.
    [[nodiscard]] uint64_t frames_written() const noexcept;
    [[nodiscard]] uint64_t frames_failed() const noexcept;

    // For the application's own signal handler: if a DashboardSink drawing
    // to standard error (Config::output null) exists, writes the escapes that
    // show the cursor and leave the alternate screen to standard error with
    // write(2). Async-signal-safe: it reads one lock-free atomic counter and
    // makes one system call, and touches no lock, no allocation and no
    // stream. A dashboard on a caller's stream is not restored by it (there
    // is no descriptor to write to safely). It does not stop the drawing
    // thread, which may draw again into the normal screen: end the process
    // after calling it.
    static void restore_terminal_after_signal() noexcept;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace lumen

#endif
