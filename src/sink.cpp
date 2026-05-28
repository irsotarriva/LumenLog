#include "lumen/sink.h"

#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

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

}  // namespace

// ── TerminalSink ─────────────────────────────────────────────────────────────

TerminalSink::Config TerminalSink::default_config() {
    Config cfg;
    for (int i = 0; i < 6; ++i) {
        cfg.colors[static_cast<size_t>(i)] =
            default_level_color(static_cast<LogLevel>(i));
    }
    return cfg;
}

TerminalSink::TerminalSink(Config cfg) : __cfg(std::move(cfg)) {}

void TerminalSink::on_log(const LogRecord& record) {
    const auto& color  = __cfg.colors[static_cast<size_t>(record.level)];
    const auto* label  = level_label(record.level);
    std::string ts     = format_time_ns(record.timestamp_ns);
    std::string tags   = format_tags(record.tags, __cfg.inline_tags);

    std::fprintf(stderr, "%s%s %s%s %s %s%s %s\n",
                 ansi_code(color), label,
                 ansi_code(AnsiColor::Default), ts.c_str(),
                 record.message.data(),
                 tags.empty() ? "" : " ",
                 tags.c_str(),
                 ansi_code(AnsiColor::Default));
}

void TerminalSink::on_metric(const MetricRecord& record) {
    std::string ts = format_time_ns(record.timestamp_ns);
    std::fprintf(stderr, "METRIC %s %s = %.6g\n",
                 ts.c_str(), record.name.data(), record.value);
}

void TerminalSink::on_progress(const ProgressRecord& record) {
    std::fprintf(stderr, "PROGRESS %s %llu/%llu\n",
                 record.label.data(),
                 static_cast<unsigned long long>(record.current),
                 static_cast<unsigned long long>(record.total));
}

void TerminalSink::flush() {
    std::fflush(stderr);
}

// ── FileSink ─────────────────────────────────────────────────────────────────

struct FileSink::Impl {
    Config cfg;
    std::unique_ptr<std::ofstream> file;
    uint64_t bytes_written{0};

    std::mutex mtx;
    std::condition_variable cv;
    std::queue<std::string> queue;
    std::atomic<bool> running{true};
    std::jthread worker;

    explicit Impl(Config c) : cfg(std::move(c)) {
        open_file();
        worker = std::jthread(&Impl::run, this);
    }

    ~Impl() {
        running.store(false, std::memory_order_relaxed);
        cv.notify_one();
        if (worker.joinable()) {
            worker.join();
        }
        if (file && file->is_open()) {
            file->close();
        }
    }

    void open_file() {
        if (file && file->is_open()) {
            file->close();
        }
        file = std::make_unique<std::ofstream>(std::string(cfg.path),
                                               std::ios::out | std::ios::app);
        bytes_written = 0;
    }

    void maybe_rotate() {
        if (bytes_written < static_cast<uint64_t>(cfg.rotate_mb) * 1024 * 1024) {
            return;
        }
        if (file && file->is_open()) {
            file->close();
        }

        std::string base(cfg.path);
        for (int i = cfg.max_files - 1; i >= 0; --i) {
            std::string old_name = base + "." + std::to_string(i);
            std::string new_name = base + "." + std::to_string(i + 1);
            if (i == 0) {
                std::rename(base.c_str(), new_name.c_str());
            } else {
                std::rename(old_name.c_str(), new_name.c_str());
            }
        }

        file = std::make_unique<std::ofstream>(base, std::ios::out | std::ios::app);
        bytes_written = 0;
    }

    void enqueue(std::string chunk) {
        {
            std::lock_guard lock(mtx);
            queue.push(std::move(chunk));
        }
        cv.notify_one();
    }

    void run() {
        while (running.load(std::memory_order_relaxed)) {
            std::string chunk;
            {
                std::unique_lock lock(mtx);
                cv.wait_for(lock, std::chrono::milliseconds(100),
                            [this] { return !queue.empty() || !running.load(std::memory_order_relaxed); });
                if (queue.empty()) continue;
                chunk = std::move(queue.front());
                queue.pop();
            }

            if (file && file->is_open()) {
                *file << chunk;
                file->flush();
                bytes_written += chunk.size();
                maybe_rotate();
            }
        }

        while (true) {
            std::string chunk;
            {
                std::lock_guard lock(mtx);
                if (queue.empty()) break;
                chunk = std::move(queue.front());
                queue.pop();
            }
            if (file && file->is_open()) {
                *file << chunk;
                file->flush();
            }
        }
    }
};

FileSink::FileSink(Config cfg) : __cfg(std::move(cfg)), __impl(std::make_unique<Impl>(__cfg)) {}

FileSink::~FileSink() = default;

void FileSink::on_log(const LogRecord& record) {
    std::string ts = format_time_ns(record.timestamp_ns);
    std::string line;
    if (__cfg.format == FileFormat::Json) {
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
    __impl->enqueue(std::move(line));
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
    __impl->enqueue(std::move(line));
}

void FileSink::on_progress(const ProgressRecord& record) {
    std::string line = "PROGRESS ";
    line += record.label;
    line += " ";
    line += std::to_string(record.current);
    line += "/";
    line += std::to_string(record.total);
    line += "\n";
    __impl->enqueue(std::move(line));
}

void FileSink::flush() {
    std::unique_lock lock(__impl->mtx);
    while (!__impl->queue.empty()) {
        lock.unlock();
        __impl->cv.notify_one();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        lock.lock();
    }
}

// ── JsonSink ──────────────────────────────────────────────────────────────────

struct JsonSink::Impl {
    std::ofstream file;
    std::mutex mtx;
    std::condition_variable cv;
    std::queue<std::string> queue;
    std::atomic<bool> running{true};
    std::jthread worker;

    explicit Impl(std::string_view path)
        : file(std::string(path), std::ios::out | std::ios::app) {
        worker = std::jthread(&Impl::run, this);
    }

    ~Impl() {
        running.store(false, std::memory_order_relaxed);
        cv.notify_one();
        if (worker.joinable()) {
            worker.join();
        }
        if (file.is_open()) {
            file.close();
        }
    }

    void enqueue(std::string chunk) {
        {
            std::lock_guard lock(mtx);
            queue.push(std::move(chunk));
        }
        cv.notify_one();
    }

    void run() {
        while (running.load(std::memory_order_relaxed)) {
            std::string chunk;
            {
                std::unique_lock lock(mtx);
                cv.wait_for(lock, std::chrono::milliseconds(100),
                            [this] { return !queue.empty() || !running.load(std::memory_order_relaxed); });
                if (queue.empty()) continue;
                chunk = std::move(queue.front());
                queue.pop();
            }

            if (file.is_open()) {
                file << chunk;
                file.flush();
            }
        }

        while (true) {
            std::string chunk;
            {
                std::lock_guard lock(mtx);
                if (queue.empty()) break;
                chunk = std::move(queue.front());
                queue.pop();
            }
            if (file.is_open()) {
                file << chunk;
                file.flush();
            }
        }
    }
};

JsonSink::JsonSink(std::string_view path) : __impl(std::make_unique<Impl>(path)) {}

JsonSink::~JsonSink() = default;

void JsonSink::on_log(const LogRecord& record) {
    std::string out;
    format_record_json(out, record);
    __impl->enqueue(std::move(out));
}

void JsonSink::on_metric(const MetricRecord& record) {
    std::string out;
    format_record_json(out, record);
    __impl->enqueue(std::move(out));
}

void JsonSink::on_progress(const ProgressRecord& record) {
    std::string out;
    format_record_json(out, record);
    __impl->enqueue(std::move(out));
}

void JsonSink::flush() {
    std::unique_lock lock(__impl->mtx);
    while (!__impl->queue.empty()) {
        lock.unlock();
        __impl->cv.notify_one();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        lock.lock();
    }
}

}  // namespace lumen
