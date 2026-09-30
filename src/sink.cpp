#include "lumen/sink.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <deque>
#include <fstream>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>

#include "lumen/detail/error.h"

#ifdef LUMEN_ENABLE_DASHBOARD
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#endif

namespace lumen {

namespace {

const char* ansi_code(AnsiColor c) {
    switch (c) {
        case AnsiColor::Default:  return "\033[0m";
        case AnsiColor::Black:    return "\033[30m";
        case AnsiColor::Red:      return "\033[31m";
        case AnsiColor::Green:    return "\033[32m";
        case AnsiColor::Yellow:   return "\033[33m";
        case AnsiColor::Blue:     return "\033[34m";
        case AnsiColor::Magenta:  return "\033[35m";
        case AnsiColor::Cyan:     return "\033[36m";
        case AnsiColor::White:    return "\033[37m";
    }
    return "";
}

const char* level_label(LogLevel level) {
    switch (level) {
        case LogLevel::TRACE: return "TRACE";
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO ";
        case LogLevel::WARN:  return "WARN ";
        case LogLevel::ERROR: return "ERROR";
        case LogLevel::FATAL: return "FATAL";
    }
    return "?????";
}

AnsiColor default_level_color(LogLevel level) {
    switch (level) {
        case LogLevel::TRACE: return AnsiColor::Default;
        case LogLevel::DEBUG: return AnsiColor::Cyan;
        case LogLevel::INFO:  return AnsiColor::Green;
        case LogLevel::WARN:  return AnsiColor::Yellow;
        case LogLevel::ERROR: return AnsiColor::Red;
        case LogLevel::FATAL: return AnsiColor::Magenta;
    }
    return AnsiColor::Default;
}

std::string format_time_ns(uint64_t ns) {
    auto dur = std::chrono::nanoseconds(ns);
    auto tp = std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(dur));
    auto t = std::chrono::system_clock::to_time_t(tp);
    auto ms = static_cast<int>((ns / 1000000) % 1000);

    std::tm tm_buf{};
#ifdef PLATFORM_WINDOWS
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d",
                  tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, ms);
    return buf;
}

std::string format_tags(const TagSet<16>& tags,
                        const std::vector<std::string_view>& inline_keys) {
    std::string result;
    for (const auto& key : inline_keys) {
        auto val = tags.find(key);
        if (!val.empty()) {
            if (!result.empty()) result += ' ';
            result += std::string(key);
            result += '=';
            result += std::string(val);
        }
    }
    return result;
}

std::string escape_json(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

void format_record_json(std::string& out, const LogRecord& record) {
    out += "{\"type\":\"log\"";
    out += ",\"ts\":" + std::to_string(record.timestamp_ns);
    out += ",\"level\":\"" + std::string(level_label(record.level)) + "\"";
    out += ",\"msg\":\"" + escape_json(record.message) + "\"";
    out += ",\"file\":\"" + escape_json(record.source.file) + "\"";
    out += ",\"line\":" + std::to_string(record.source.line);
    for (const auto& e : record.tags) {
        if (e.key.empty()) continue;
        out += ",\"" + escape_json(e.key) + "\":\"" + escape_json(e.value) + "\"";
    }
    out += "}\n";
}

void format_record_json(std::string& out, const MetricRecord& record) {
    out += "{\"type\":\"metric\"";
    out += ",\"ts\":" + std::to_string(record.timestamp_ns);
    out += ",\"name\":\"" + escape_json(record.name) + "\"";
    out += ",\"value\":" + std::to_string(record.value);
    for (const auto& e : record.tags) {
        if (e.key.empty()) continue;
        out += ",\"" + escape_json(e.key) + "\":\"" + escape_json(e.value) + "\"";
    }
    out += "}\n";
}

void format_record_json(std::string& out, const ProgressRecord& record) {
    out += "{\"type\":\"progress\"";
    out += ",\"ts\":" + std::to_string(record.timestamp_ns);
    out += ",\"label\":\"" + escape_json(record.label) + "\"";
    out += ",\"current\":" + std::to_string(record.current);
    out += ",\"total\":" + std::to_string(record.total);
    for (const auto& e : record.tags) {
        if (e.key.empty()) continue;
        out += ",\"" + escape_json(e.key) + "\":\"" + escape_json(e.value) + "\"";
    }
    out += "}\n";
}

// Writes chunks on a sink-owned thread so that on_* never blocks on I/O.
// wait_idle() returns once every chunk enqueued before the call has been
// handed to `write` (and `write` has returned). The destructor drains the
// queue before joining.
class BackgroundWriter {
public:
    explicit BackgroundWriter(std::function<void(const std::string&)> write)
        : write_(std::move(write)), worker_([this] { run_(); }) {}

    ~BackgroundWriter() {
        {
            std::lock_guard lock(mtx_);
            stopping_ = true;
        }
        cv_.notify_one();
        worker_.join();
    }

    BackgroundWriter(const BackgroundWriter&) = delete;
    BackgroundWriter& operator=(const BackgroundWriter&) = delete;
    BackgroundWriter(BackgroundWriter&&) = delete;
    BackgroundWriter& operator=(BackgroundWriter&&) = delete;

    void enqueue(std::string chunk) {
        {
            std::lock_guard lock(mtx_);
            queue_.push(std::move(chunk));
            ++enqueued_;
        }
        cv_.notify_one();
    }

    void wait_idle() {
        std::unique_lock lock(mtx_);
        const uint64_t target = enqueued_;
        idle_cv_.wait(lock, [this, target] { return written_ >= target; });
    }

private:
    void run_() {
        std::unique_lock lock(mtx_);
        while (true) {
            cv_.wait(lock, [this] { return !queue_.empty() || stopping_; });
            if (queue_.empty()) {
                return;  // stopping and fully drained
            }
            std::string chunk = std::move(queue_.front());
            queue_.pop();
            lock.unlock();
            write_(chunk);
            lock.lock();
            ++written_;
            idle_cv_.notify_all();
        }
    }

    std::function<void(const std::string&)> write_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::condition_variable idle_cv_;
    std::queue<std::string> queue_;
    uint64_t enqueued_ = 0;
    uint64_t written_ = 0;
    bool stopping_ = false;
    std::thread worker_;  // last: starts after every other member is ready
};

}  // namespace

// ── TerminalSink ─────────────────────────────────────────────────────────────

#ifdef LUMEN_ENABLE_DASHBOARD

namespace {

constexpr size_t kMaxScrollback = 512;
constexpr size_t kMaxMetricHistory = 128;

struct MetricHistory {
    std::vector<double> values;
    double min_val = 0.0;
    double max_val = 0.0;
};

struct ScrollLine {
    std::string text;
    AnsiColor color = AnsiColor::Default;
};

}  // namespace

struct TerminalSink::DashboardState {
    std::unordered_map<std::string, MetricHistory> metrics;
    std::unordered_map<uint64_t, ProgressRecord> progress;
    std::deque<ScrollLine> scrollback;

    void record_metric(std::string_view name, double value) {
        auto& hist = metrics[std::string(name)];
        if (hist.values.size() >= kMaxMetricHistory) {
            hist.values.erase(hist.values.begin());
        }
        hist.values.push_back(value);
        hist.min_val = *std::min_element(hist.values.begin(), hist.values.end());
        hist.max_val = *std::max_element(hist.values.begin(), hist.values.end());
    }

    void record_progress(const ProgressRecord& record) {
        if (record.current >= record.total) {
            progress.erase(record.id);
        } else {
            progress[record.id] = record;
        }
    }

    void record_log(ScrollLine line) {
        if (scrollback.size() >= kMaxScrollback) {
            scrollback.pop_front();
        }
        scrollback.push_back(std::move(line));
    }

    std::string render(const TerminalSink::Config& cfg) {
        try {
            return render_impl_(cfg);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "lumen: FTXUI render exception: %s\n", e.what());
            return last_rendered_;
        } catch (...) {
            std::fprintf(stderr, "lumen: FTXUI render unknown exception\n");
            return last_rendered_;
        }
    }

private:
    std::string last_rendered_;

    std::string render_impl_(const TerminalSink::Config& cfg) {
        using namespace ftxui;

        const int dashboard_h = cfg.dashboard_height > 0 ? cfg.dashboard_height : 5;

        Elements dash_rows;

        for (const auto& name_sv : cfg.dashboard_metrics) {
            std::string name(name_sv);
            auto it = metrics.find(name);
            if (it == metrics.end() || it->second.values.empty()) {
                dash_rows.push_back(
                    hbox({text(" " + name + " ") | bold | color(Color::GrayDark),
                          text(" (no data)")}));
                continue;
            }

            const auto& hist = it->second;
            const double latest = hist.values.back();
            char val_buf[32];
            std::snprintf(val_buf, sizeof(val_buf), "%.3g", latest);

            std::vector<int> graph_vals;
            graph_vals.reserve(hist.values.size());
            for (double v : hist.values) {
                double range = hist.max_val - hist.min_val;
                if (range < 1e-9) range = 1.0;
                double norm = (v - hist.min_val) / range;
                graph_vals.push_back(static_cast<int>(norm * 100.0));
            }

            auto sparkline = graph([vals = std::move(graph_vals)](int width, int height) {
                                 std::vector<int> out;
                                 out.reserve(static_cast<size_t>(width));
                                 if (vals.empty()) return out;
                                 for (int i = 0; i < width; ++i) {
                                     size_t idx = static_cast<size_t>(i) * (vals.size() - 1)
                                         / static_cast<size_t>(width > 1 ? width - 1 : 1);
                                     out.push_back(vals[idx] * (height - 1) / 100);
                                 }
                                 return out;
                             }) |
                             size(WIDTH, GREATER_THAN, 20) |
                             size(HEIGHT, EQUAL, 1);

            dash_rows.push_back(
                hbox({text(" " + name + " ") | bold,
                      text(std::string(val_buf)) | color(Color::Cyan),
                      text(" "),
                      sparkline | flex}));
        }

        for (const auto& [id, rec] : progress) {
            double ratio = rec.total > 0
                ? static_cast<double>(rec.current) / static_cast<double>(rec.total)
                : 0.0;
            ratio = std::clamp(ratio, 0.0, 1.0);

            char prog_buf[64];
            std::snprintf(prog_buf, sizeof(prog_buf), " %llu/%llu",
                          static_cast<unsigned long long>(rec.current),
                          static_cast<unsigned long long>(rec.total));

            dash_rows.push_back(
                hbox({text(" " + std::string(rec.label) + " ") | bold,
                      gauge(static_cast<float>(ratio)) | flex,
                      text(std::string(prog_buf))}));
        }

        if (dash_rows.empty()) {
            dash_rows.push_back(text(" (no dashboard data)") | dim);
        }

        Element dashboard = vbox(std::move(dash_rows)) |
                            border |
                            size(HEIGHT, GREATER_THAN, static_cast<int>(dashboard_h));

        Elements log_lines;
        for (const auto& line : scrollback) {
            log_lines.push_back(text(line.text));
        }

        Element log_region;
        if (log_lines.empty()) {
            log_region = text(" (no log output)") | dim | flex;
        } else {
            log_region = vbox(std::move(log_lines)) | flex;
        }

        Element root = vbox({
            dashboard,
            separator(),
            log_region | flex,
        });

        auto dim = Dimension::Full();
        auto screen = Screen::Create(dim, dim);
        Render(screen, root);

        std::string output;
        output.reserve(8192);

        output += "\033[2J\033[H";

        std::string screen_str = screen.ToString();
        for (char c : screen_str) {
            if (c == '\n') {
                output += "\033[K\n";
            } else {
                output += c;
            }
        }
        output += "\033[K";

        last_rendered_ = output;
        return output;
    }
};

#else

struct TerminalSink::DashboardState {
};

#endif

TerminalSink::Config TerminalSink::default_config() {
    Config cfg;
    for (int i = 0; i < 6; ++i) {
        cfg.colors[static_cast<size_t>(i)] =
            default_level_color(static_cast<LogLevel>(i));
    }
    return cfg;
}

TerminalSink::TerminalSink(Config cfg)
    : cfg_(std::move(cfg)) {
#ifdef LUMEN_ENABLE_DASHBOARD
    if (cfg_.enable_dashboard) {
        dashboard_ = std::make_unique<DashboardState>();
        std::fprintf(stderr, "\033[?1049h");
    }
#endif
}

TerminalSink::~TerminalSink() {
#ifdef LUMEN_ENABLE_DASHBOARD
    if (dashboard_) {
        std::fprintf(stderr, "\033[?1049l");
    }
#endif
}

void TerminalSink::on_log(const LogRecord& record) {
#ifdef LUMEN_ENABLE_DASHBOARD
    if (dashboard_) {
        const auto& color = cfg_.colors[static_cast<size_t>(record.level)];
        std::string ts = format_time_ns(record.timestamp_ns);
        std::string tags = format_tags(record.tags, cfg_.inline_tags);

        std::string line;
        line += level_label(record.level);
        line += " ";
        line += ts;
        line += " ";
        line += record.message;
        if (!tags.empty()) {
            line += " ";
            line += tags;
        }
        dashboard_->record_log(ScrollLine{std::move(line), color});
        std::string rendered = dashboard_->render(cfg_);
        std::fprintf(stderr, "%s", rendered.c_str());
        return;
    }
#endif
    const auto& color  = cfg_.colors[static_cast<size_t>(record.level)];
    const auto* label  = level_label(record.level);
    std::string ts     = format_time_ns(record.timestamp_ns);
    std::string tags   = format_tags(record.tags, cfg_.inline_tags);

    std::fprintf(stderr, "%s%s %s%s %.*s %s%s %s\n",
                 ansi_code(color), label,
                 ansi_code(AnsiColor::Default), ts.c_str(),
                 static_cast<int>(record.message.size()), record.message.data(),
                 tags.empty() ? "" : " ",
                 tags.c_str(),
                 ansi_code(AnsiColor::Default));
}

void TerminalSink::on_metric(const MetricRecord& record) {
#ifdef LUMEN_ENABLE_DASHBOARD
    if (dashboard_) {
        dashboard_->record_metric(record.name, record.value);
        std::string rendered = dashboard_->render(cfg_);
        std::fprintf(stderr, "%s", rendered.c_str());
        return;
    }
#endif
    std::string ts = format_time_ns(record.timestamp_ns);
    std::fprintf(stderr, "METRIC %s %.*s = %.6g\n",
                 ts.c_str(), static_cast<int>(record.name.size()), record.name.data(),
                 record.value);
}

void TerminalSink::on_progress(const ProgressRecord& record) {
#ifdef LUMEN_ENABLE_DASHBOARD
    if (dashboard_) {
        dashboard_->record_progress(record);
        std::string rendered = dashboard_->render(cfg_);
        std::fprintf(stderr, "%s", rendered.c_str());
        return;
    }
#endif
    std::fprintf(stderr, "PROGRESS %.*s %llu/%llu\n",
                 static_cast<int>(record.label.size()), record.label.data(),
                 static_cast<unsigned long long>(record.current),
                 static_cast<unsigned long long>(record.total));
}

void TerminalSink::flush() {
#ifdef LUMEN_ENABLE_DASHBOARD
    if (dashboard_) {
        std::string rendered = dashboard_->render(cfg_);
        std::fprintf(stderr, "%s", rendered.c_str());
    }
#endif
    std::fflush(stderr);
}

// ── FileSink ─────────────────────────────────────────────────────────────────

struct FileSink::Impl {
    Config cfg;
    std::string path;  // owned copy: cfg.path is a view into the caller's string
    std::unique_ptr<std::ofstream> file;
    uint64_t bytes_written{0};
    BackgroundWriter writer;  // last: joined before the file is closed

    explicit Impl(Config c)
        : cfg(c), path(c.path), writer([this](const std::string& chunk) { write(chunk); }) {
        cfg.path = path;
        open_file();
    }

    ~Impl() = default;

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    void open_file() {
        file = std::make_unique<std::ofstream>(path, std::ios::out | std::ios::app);
        if (!file->is_open()) {
            std::fprintf(stderr, "lumen: failed to open log file '%s'\n", path.c_str());
        }
        bytes_written = 0;
    }

    void maybe_rotate() {
        if (bytes_written < static_cast<uint64_t>(cfg.rotate_mb) * 1024 * 1024) {
            return;
        }
        if (file && file->is_open()) {
            file->close();
        }

        for (int i = cfg.max_files - 1; i >= 0; --i) {
            std::string old_name = path + "." + std::to_string(i);
            std::string new_name = path + "." + std::to_string(i + 1);
            const char* from = (i == 0) ? path.c_str() : old_name.c_str();
            if (std::rename(from, new_name.c_str()) != 0 && i == 0) {
                std::fprintf(stderr, "lumen: failed to rename '%s' -> '%s'\n",
                             from, new_name.c_str());
            }
        }

        open_file();
    }

    // Runs on the writer thread only.
    void write(const std::string& chunk) {
        if (!file || !file->is_open()) {
            return;
        }
        *file << chunk;
        file->flush();
        if (!file->good()) {
            std::fprintf(stderr, "lumen: I/O error writing to log file '%s'\n", path.c_str());
        }
        bytes_written += chunk.size();
        maybe_rotate();
    }
};

FileSink::FileSink(Config cfg) : cfg_(std::move(cfg)), impl_(std::make_unique<Impl>(cfg_)) {}

FileSink::~FileSink() = default;

void FileSink::on_log(const LogRecord& record) {
    std::string ts = format_time_ns(record.timestamp_ns);
    std::string line;
    if (cfg_.format == FileFormat::Json) {
        line = "LOG ";
    }
    line += ts;
    line += " [";
    line += level_label(record.level);
    line += "] ";
    line += record.message;
    line += " (";
    line += record.source.file;
    line += ":";
    line += std::to_string(record.source.line);
    line += ")";

    for (const auto& e : record.tags) {
        if (e.key.empty()) continue;
        line += " ";
        line += e.key;
        line += "=";
        line += e.value;
    }
    line += "\n";
    impl_->writer.enqueue(std::move(line));
}

void FileSink::on_metric(const MetricRecord& record) {
    std::string ts = format_time_ns(record.timestamp_ns);
    std::string line = ts + " " + std::string(record.name) +
                       "=" + std::to_string(record.value);
    for (const auto& e : record.tags) {
        if (e.key.empty()) continue;
        line += " ";
        line += e.key;
        line += "=";
        line += e.value;
    }
    line += "\n";
    impl_->writer.enqueue(std::move(line));
}

void FileSink::on_progress(const ProgressRecord& record) {
    std::string line = "PROGRESS ";
    line += record.label;
    line += " ";
    line += std::to_string(record.current);
    line += "/";
    line += std::to_string(record.total);
    line += "\n";
    impl_->writer.enqueue(std::move(line));
}

void FileSink::flush() {
    impl_->writer.wait_idle();
}

// ── JsonSink ──────────────────────────────────────────────────────────────────

struct JsonSink::Impl {
    std::ofstream file;
    BackgroundWriter writer;  // last: joined before the file is closed

    explicit Impl(std::string_view path)
        : file(std::string(path), std::ios::out | std::ios::app),
          writer([this](const std::string& chunk) { write(chunk); }) {
        if (!file.is_open()) {
            std::fprintf(stderr, "lumen: failed to open JSON log file '%s'\n",
                         std::string(path).c_str());
        }
    }

    // Runs on the writer thread only.
    void write(const std::string& chunk) {
        if (!file.is_open()) {
            return;
        }
        file << chunk;
        file.flush();
        if (!file.good()) {
            std::fprintf(stderr, "lumen: I/O error writing to JSON log file\n");
        }
    }
};

JsonSink::JsonSink(std::string_view path) : impl_(std::make_unique<Impl>(path)) {}

JsonSink::~JsonSink() = default;

void JsonSink::on_log(const LogRecord& record) {
    std::string out;
    format_record_json(out, record);
    impl_->writer.enqueue(std::move(out));
}

void JsonSink::on_metric(const MetricRecord& record) {
    std::string out;
    format_record_json(out, record);
    impl_->writer.enqueue(std::move(out));
}

void JsonSink::on_progress(const ProgressRecord& record) {
    std::string out;
    format_record_json(out, record);
    impl_->writer.enqueue(std::move(out));
}

void JsonSink::flush() {
    impl_->writer.wait_idle();
}

}  // namespace lumen
