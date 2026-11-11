#include "dashboard_frame.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/color.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/terminal.hpp>

namespace lumen::detail {

namespace {

constexpr uint16_t fallback_width = 80;
constexpr uint16_t fallback_height = 24;
constexpr std::size_t sparkline_width = 32;
constexpr std::size_t border_rows = 2;

// U+2581 LOWER ONE EIGHTH BLOCK ... U+2588 FULL BLOCK, in UTF-8.
constexpr std::array<std::string_view, 8> spark_levels = {
    "\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83", "\xe2\x96\x84",
    "\xe2\x96\x85", "\xe2\x96\x86", "\xe2\x96\x87", "\xe2\x96\x88",
};

[[nodiscard]] ftxui::Color to_ftxui(AnsiColor color) {
    switch (color) {
        case AnsiColor::Default: return ftxui::Color::Default;
        case AnsiColor::Black:   return ftxui::Color::Black;
        case AnsiColor::Red:     return ftxui::Color::Red;
        case AnsiColor::Green:   return ftxui::Color::Green;
        case AnsiColor::Yellow:  return ftxui::Color::Yellow;
        case AnsiColor::Blue:    return ftxui::Color::Blue;
        case AnsiColor::Magenta: return ftxui::Color::Magenta;
        case AnsiColor::Cyan:    return ftxui::Color::Cyan;
        case AnsiColor::White:   return ftxui::Color::White;
    }
    return ftxui::Color::Default;
}

// The last values of the series as block characters, lowest to highest.
[[nodiscard]] std::string sparkline(const DashboardModel::MetricSeries& series) {
    std::string out;
    const std::size_t count = std::min(series.values.size(), sparkline_width);
    const double range = series.max - series.min;
    for (std::size_t i = series.values.size() - count; i < series.values.size(); ++i) {
        const double value = series.values.at(i);
        if (!std::isfinite(value) || !std::isfinite(range)) {
            out += ' ';
            continue;
        }
        const double scaled = range > 0.0 ? (value - series.min) / range : 0.5;
        const auto level =
            static_cast<std::size_t>(std::lround(std::clamp(scaled, 0.0, 1.0) * 7.0));
        out += spark_levels.at(std::min<std::size_t>(level, spark_levels.size() - 1));
    }
    return out;
}

[[nodiscard]] ftxui::Element metric_row(const DashboardModel::MetricSeries& series) {
    ftxui::Element name = ftxui::text(" " + series.name + " ") | ftxui::bold;
    if (series.values.empty()) {
        return ftxui::hbox({std::move(name), ftxui::text("(no data)") | ftxui::dim});
    }
    return ftxui::hbox({
        std::move(name),
        ftxui::text(std::format("{:.6g}", series.values.back())) | ftxui::color(ftxui::Color::Cyan),
        ftxui::text(std::format("  [{:.4g}, {:.4g}]  ", series.min, series.max)) | ftxui::dim,
        ftxui::text(sparkline(series)),
    });
}

[[nodiscard]] ftxui::Element progress_row(const DashboardModel::ProgressBar& bar) {
    ftxui::Element label = ftxui::text(" " + bar.label + " ") | ftxui::bold;
    if (bar.total == 0) {
        return ftxui::hbox({std::move(label), ftxui::text(std::format("{} done", bar.current))});
    }
    const double ratio =
        std::clamp(static_cast<double>(bar.current) / static_cast<double>(bar.total), 0.0, 1.0);
    return ftxui::hbox({
        std::move(label),
        ftxui::gauge(static_cast<float>(ratio)) | ftxui::flex,
        ftxui::text(std::format(" {}/{}", bar.current, bar.total)),
    });
}

// Metrics first, then bars, at most `max_rows` rows.
[[nodiscard]] ftxui::Elements panel_rows(const DashboardModel& model, std::size_t max_rows) {
    ftxui::Elements rows;
    const std::size_t room = std::max<std::size_t>(max_rows, 1);
    for (const DashboardModel::MetricSeries& series : model.metrics()) {
        rows.push_back(metric_row(series));
    }
    for (const DashboardModel::ProgressBar& bar : model.progress_bars()) {
        rows.push_back(progress_row(bar));
    }
    if (rows.size() > room) {
        const std::size_t hidden = rows.size() - room + 1;
        rows.resize(room - 1);
        rows.push_back(ftxui::text(std::format(" ... {} more", hidden)) | ftxui::dim);
    }
    if (rows.empty()) {
        rows.push_back(ftxui::text(" (no metrics or progress yet)") | ftxui::dim);
    }
    return rows;
}

// The newest log lines that fit in `room` rows, oldest at the top.
[[nodiscard]] ftxui::Elements log_rows(const DashboardModel& model, std::size_t room,
                                       const LevelColors& colors) {
    ftxui::Elements rows;
    const auto& lines = model.scrollback();
    const std::size_t first = lines.size() > room ? lines.size() - room : 0;
    for (std::size_t i = first; i < lines.size(); ++i) {
        const DashboardModel::LogLine& line = lines.at(i);
        rows.push_back(ftxui::text(line.text) |
                       ftxui::color(to_ftxui(color_for(colors, line.level))));
    }
    return rows;
}

struct Size {
    uint16_t width = 0;
    uint16_t height = 0;
};

[[nodiscard]] uint16_t or_fallback(int measured, uint16_t fallback) {
    constexpr int largest = std::numeric_limits<uint16_t>::max();
    return measured > 0 ? static_cast<uint16_t>(std::min(measured, largest)) : fallback;
}

[[nodiscard]] Size frame_size(const FrameSettings& settings) {
    if (settings.width > 0 && settings.height > 0) {
        return {settings.width, settings.height};
    }
    const ftxui::Dimensions terminal = ftxui::Terminal::Size();
    return {settings.width > 0 ? settings.width : or_fallback(terminal.dimx, fallback_width),
            settings.height > 0 ? settings.height : or_fallback(terminal.dimy, fallback_height)};
}

// FTXUI separates rows with "\r\n" or "\n" depending on its version. Each row
// is ended here by resetting the colour and clearing the rest of the line, so
// that shorter content leaves nothing of the previous frame behind.
[[nodiscard]] std::string with_line_clears(std::string_view screen) {
    std::string out;
    out.reserve(screen.size() + screen.size() / 8);
    std::size_t start = 0;
    while (start <= screen.size()) {
        const std::size_t end = std::min(screen.find('\n', start), screen.size());
        std::string_view row = screen.substr(start, end - start);
        if (!row.empty() && row.back() == '\r') {
            row.remove_suffix(1);
        }
        out += row;
        out += "\x1b[0m\x1b[K";
        if (end == screen.size()) {
            break;
        }
        out += "\r\n";
        start = end + 1;
    }
    return out;
}

}  // namespace

std::string render_frame(const DashboardModel& model, const FrameSettings& settings) {
    const Size size = frame_size(settings);
    ftxui::Elements panel = panel_rows(model, settings.panel_rows);
    const std::size_t panel_height = panel.size() + border_rows;
    const std::size_t log_room = size.height > panel_height ? size.height - panel_height : 0;
    const ftxui::Element root = ftxui::vbox({
        ftxui::vbox(std::move(panel)) | ftxui::border,
        ftxui::vbox(log_rows(model, log_room, settings.colors)),
    });
    ftxui::Screen screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(size.width),
                                                 ftxui::Dimension::Fixed(size.height));
    ftxui::Render(screen, root);
    return with_line_clears(screen.ToString());
}

}  // namespace lumen::detail
