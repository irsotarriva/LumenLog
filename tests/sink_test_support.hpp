#ifndef LUMEN_TESTS_SINK_TEST_SUPPORT_HPP
#define LUMEN_TESTS_SINK_TEST_SUPPORT_HPP

// Shared by the sink tests: records with fixed contents, a temporary
// directory, and streams that block or fail on demand.

#include <gtest/gtest.h>

#include <atomic>
#include <concepts>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <memory>
#include <mutex>
#include <ostream>
#include <random>
#include <sstream>
#include <streambuf>
#include <string>
#include <string_view>
#include <vector>

#include "lumen/record.hpp"
#include "lumen/sink.hpp"

namespace lumen::test {

template <typename R>
concept AnyRecord =
    std::same_as<R, LogRecord> || std::same_as<R, MetricRecord> || std::same_as<R, ProgressRecord>;

// Hands `record` to the sink's matching callback.
inline void deliver(Sink& sink, const LogRecord& record) {
    sink.on_log(record);
}
inline void deliver(Sink& sink, const MetricRecord& record) {
    sink.on_metric(record);
}
inline void deliver(Sink& sink, const ProgressRecord& record) {
    sink.on_progress(record);
}

// 2026-11-11T10:00:00.123456789Z
inline constexpr uint64_t fixed_time_ns = 1'794'391'200'123'456'789;
inline constexpr std::string_view fixed_time_iso = "2026-11-11T10:00:00.123456789Z";
inline constexpr uint32_t fixed_thread = 3;

[[nodiscard]] inline LogRecord log_record(std::string_view message,
                                          LogLevel level = LogLevel::Info) {
    LogRecord record;
    record.level = level;
    record.message = message;
    record.timestamp_ns = fixed_time_ns;
    record.thread_id = fixed_thread;
    record.source = {.file = "main.cpp", .function = "run", .class_name = "", .line = 42};
    return record;
}

[[nodiscard]] inline MetricRecord metric_record(std::string_view name, double value) {
    MetricRecord record;
    record.name = name;
    record.value = value;
    record.timestamp_ns = fixed_time_ns;
    record.thread_id = fixed_thread;
    return record;
}

[[nodiscard]] inline ProgressRecord progress_record(std::string_view label, uint64_t current,
                                                    uint64_t total, uint64_t id = 7) {
    ProgressRecord record;
    record.id = id;
    record.label = label;
    record.current = current;
    record.total = total;
    record.timestamp_ns = fixed_time_ns;
    record.thread_id = fixed_thread;
    return record;
}

// A fresh directory under the system's temporary directory, removed with
// everything in it when the object is destroyed.
class TempDir {
public:
    TempDir() {
        std::random_device seed;
        std::mt19937_64 random(seed());
        path_ = std::filesystem::temp_directory_path() / ("lumen_test_" + std::to_string(random()));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }
    [[nodiscard]] std::filesystem::path operator/(std::string_view name) const {
        return path_ / name;
    }

private:
    std::filesystem::path path_;
};

[[nodiscard]] inline std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

[[nodiscard]] inline std::vector<std::string> read_lines(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);) {
        lines.push_back(line);
    }
    return lines;
}

// A stream buffer that fails every write.
class FailingBuffer : public std::streambuf {
protected:
    int_type overflow(int_type /*ch*/) override { return traits_type::eof(); }
    std::streamsize xsputn(const char_type* /*s*/, std::streamsize /*n*/) override { return 0; }
};

// A stream buffer that collects what is written, but holds the first write
// until open() is called. entered() tells whether a writer is waiting in it.
class GateBuffer : public std::streambuf {
public:
    void open() {
        {
            const std::scoped_lock lock(mutex_);
            open_ = true;
        }
        changed_.notify_all();
    }
    // Waits until a write is held at the gate.
    void wait_until_entered() {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [&] { return entered_; });
    }
    [[nodiscard]] std::string text() const {
        const std::scoped_lock lock(mutex_);
        return text_;
    }

protected:
    std::streamsize xsputn(const char_type* s, std::streamsize n) override {
        std::unique_lock lock(mutex_);
        entered_ = true;
        changed_.notify_all();
        changed_.wait(lock, [&] { return open_; });
        text_.append(s, static_cast<std::size_t>(n));
        return n;
    }
    int_type overflow(int_type ch) override {
        if (traits_type::eq_int_type(ch, traits_type::eof())) {
            return traits_type::not_eof(ch);
        }
        const char c = traits_type::to_char_type(ch);
        return xsputn(&c, 1) == 1 ? ch : traits_type::eof();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;  // waits on mutex_
    bool open_ = false;                // guarded by mutex_
    bool entered_ = false;             // guarded by mutex_
    std::string text_;                 // guarded by mutex_
};

// A std::ostream over a buffer it owns, handed out as the shared_ptr that
// the sink configs take.
template <std::derived_from<std::streambuf> Buffer>
struct StreamOver {
    Buffer buffer;
    std::ostream stream{&buffer};
};

template <std::derived_from<std::streambuf> Buffer>
[[nodiscard]] std::shared_ptr<StreamOver<Buffer>> make_stream_over() {
    return std::make_shared<StreamOver<Buffer>>();
}

// The ostream inside `owner`, sharing its lifetime.
template <std::derived_from<std::streambuf> Buffer>
[[nodiscard]] std::shared_ptr<std::ostream> as_ostream(
    const std::shared_ptr<StreamOver<Buffer>>& owner) {
    return {owner, &owner->stream};
}

}  // namespace lumen::test

#endif
