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

#include "lumen/core.hpp"
#include "lumen/dashboard_model.hpp"
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
// The terminal. create() switches the terminal to its alternate screen and
// hides the cursor; the destructor shows the cursor and switches back, which
// restores what was on the screen before. A fatal record (LUMEN_LOG_FATAL)
// is followed by flush() and std::terminate, so no destructor runs: when
// flush() follows a Fatal record, the sink stops drawing, restores the
// terminal at once and writes the log lines it holds (the fatal one last) as
// plain text to the normal screen, where they stay after the process ends.
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
        // Smallest time between two frames; at least 1 ms.
        std::chrono::milliseconds refresh_interval{100};
        // Frame size in cells; 0: the terminal's size.
        uint16_t width = 0;
        uint16_t height = 0;
        // Most rows the metric and progress panel may take.
        std::size_t panel_rows = 8;
        LevelColors colors = default_level_colors;
        // The time column of the log lines; as TerminalSink::Config.
        std::string time_format = "%H:%M:%S";
        TimeZone time_zone = TimeZone::Local;
        std::vector<std::string> inline_tags;
        // Receives the first failed frame write and the first failed draw.
        DiagnosticHandler diagnostics = stderr_diagnostics();
    };

    // Fails with LumenError::NotATerminal if the destination is not a
    // terminal and assume_terminal is false, and with
    // LumenError::InvalidSinkConfig for an invalid time_format or a
    // refresh_interval below 1 ms.
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

    // Frames written without an error, and frames lost to a failed draw or
    // write. Safe to call from any thread.
    [[nodiscard]] uint64_t frames_written() const noexcept;
    [[nodiscard]] uint64_t frames_failed() const noexcept;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace lumen

#endif
