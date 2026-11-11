// A sink of your own: comma-separated lines for log and metric records,
// written to a stream by a thread the sink owns.
//
// The contract (lumen/sink.hpp): on_log, on_metric and on_progress run on the
// Core's dispatch thread, which serves every sink, so they must not wait for
// I/O. This sink formats the line, appends it to a bounded queue under a
// mutex that the writer thread holds only to take the whole queue, and
// returns. flush() is where waiting is allowed: it returns once every line
// queued before it has been written and the stream flushed.
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <format>
#include <iostream>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include "lumen/lumen.hpp"

namespace {

[[nodiscard]] std::string_view level_name(lumen::LogLevel level) {
    switch (level) {
        case lumen::LogLevel::Trace: return "TRACE";
        case lumen::LogLevel::Debug: return "DEBUG";
        case lumen::LogLevel::Info:  return "INFO";
        case lumen::LogLevel::Warn:  return "WARN";
        case lumen::LogLevel::Error: return "ERROR";
        case lumen::LogLevel::Fatal: return "FATAL";
    }
    return "UNKNOWN";
}

// A CSV field: quoted, with quotes doubled (RFC 4180).
[[nodiscard]] std::string csv_field(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        out += c;
        if (c == '"') {
            out += '"';
        }
    }
    out += '"';
    return out;
}

[[nodiscard]] std::string tags_field(const lumen::RecordTags& tags) {
    std::string out;
    for (const auto& [key, value] : tags) {
        out += std::format("{}{}={}", out.empty() ? "" : ";", key, value);
    }
    return csv_field(out);
}

class CsvSink final : public lumen::Sink {
    // Only create() can make the key, so a CsvSink always has its thread.
    class Key {
        friend class CsvSink;
        Key() = default;
    };

public:
    // `out` must outlive the sink. Fails with the error of std::thread when
    // the writer thread cannot be started.
    [[nodiscard]] static std::expected<std::unique_ptr<CsvSink>, std::error_code> create(
        std::ostream& out, std::size_t max_queued_lines) {
        try {
            return std::make_unique<CsvSink>(Key{}, out, max_queued_lines);
        } catch (const std::system_error& e) {
            return std::unexpected(e.code());
        }
    }

    CsvSink(Key /*key*/, std::ostream& out, std::size_t max_queued_lines)
        : out_(out), max_queued_lines_(max_queued_lines), writer_([this] { write_lines_(); }) {}

    ~CsvSink() override {
        {
            const std::scoped_lock lock(mutex_);
            stopping_ = true;
        }
        work_.notify_one();
        writer_.join();  // the writer empties the queue before it returns
    }

    CsvSink(const CsvSink&) = delete;
    CsvSink& operator=(const CsvSink&) = delete;
    CsvSink(CsvSink&&) = delete;
    CsvSink& operator=(CsvSink&&) = delete;

    void on_log(const lumen::LogRecord& record) override {
        enqueue_(std::format("log,{},{},{}", level_name(record.level), csv_field(record.message),
                             tags_field(record.tags)));
    }

    void on_metric(const lumen::MetricRecord& record) override {
        enqueue_(std::format("metric,{},{},{}", csv_field(record.name), record.value,
                             tags_field(record.tags)));
    }

    // on_progress is not overridden: the base class ignores progress records.

    void flush() override {
        std::unique_lock lock(mutex_);
        const uint64_t target = accepted_;
        written_cv_.wait(lock, [&] { return written_ >= target; });
    }

    [[nodiscard]] uint64_t dropped() const {
        const std::scoped_lock lock(mutex_);
        return dropped_;
    }

private:
    void enqueue_(std::string line) {
        {
            const std::scoped_lock lock(mutex_);
            if (queue_.size() >= max_queued_lines_) {
                ++dropped_;  // never wait for the writer: count the line and move on
                return;
            }
            queue_.push_back(std::move(line));
            ++accepted_;
        }
        work_.notify_one();
    }

    void write_lines_() {
        std::unique_lock lock(mutex_);
        while (true) {
            work_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) {
                return;  // stopping, and nothing left to write
            }
            std::deque<std::string> batch;
            batch.swap(queue_);
            lock.unlock();
            for (const std::string& line : batch) {
                out_ << line << '\n';
            }
            out_.flush();
            lock.lock();
            written_ += batch.size();
            written_cv_.notify_all();
        }
    }

    std::ostream& out_;  // written only by the writer thread
    const std::size_t max_queued_lines_;
    mutable std::mutex mutex_;
    std::condition_variable work_;        // waits on mutex_
    std::condition_variable written_cv_;  // waits on mutex_
    std::deque<std::string> queue_;       // guarded by mutex_
    uint64_t accepted_ = 0;               // guarded by mutex_
    uint64_t written_ = 0;                // guarded by mutex_
    uint64_t dropped_ = 0;                // guarded by mutex_
    bool stopping_ = false;               // guarded by mutex_
    // std::thread rather than std::jthread: Apple's libc++ lacks jthread.
    // Declared last, so that it starts after every member it uses exists.
    std::thread writer_;
};

}  // namespace

int main() {
    auto created = CsvSink::create(std::cout, 1000);
    if (!created) {
        std::cerr << "cannot create the CSV sink: " << created.error().message() << '\n';
        return 1;
    }
    const CsvSink* const csv = created->get();  // non-owning: the Core owns the sink
    if (const auto id = lumen::core().add_sink(std::move(*created)); !id) {
        std::cerr << "cannot register the CSV sink: " << id.error().message() << '\n';
        return 1;
    }

    LUMEN_LOG_INFO("run started").tag("run", 7);
    LUMEN_LOG_WARN("say \"cheese\"").tag("camera", "front");
    lumen::metric("exposure_ms", 12.5).tag("camera", "front");
    auto bar = lumen::progress("frames", 2);  // progress records: ignored by this sink
    bar.finish();

    lumen::core().flush();
    std::cout << "dropped: " << csv->dropped() << '\n';
    return 0;
}
