#ifndef LUMEN_TESTS_SINK_TEST_SUPPORT_HPP
#define LUMEN_TESTS_SINK_TEST_SUPPORT_HPP

// Shared by the sink tests: records with fixed contents, a temporary
// directory, streams that block or fail on demand, and scoped changes of the
// environment, the time zone and standard error.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <concepts>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <random>
#include <sstream>
#include <streambuf>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef PLATFORM_WINDOWS
#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

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

// The lines of `text`, without their '\n'. Every line of a sink ends with
// '\n' and none holds a carriage return, so a missing final '\n' or a "\r\n"
// fails the calling test.
[[nodiscard]] inline std::vector<std::string> split_lines(std::string_view text) {
    EXPECT_TRUE(text.empty() || text.back() == '\n') << "the last line has no '\\n'";
    EXPECT_EQ(text.find('\r'), std::string_view::npos) << "a carriage return";
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = std::min(text.find('\n', start), text.size());
        lines.emplace_back(text.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

// The lines of the file at `path`, as split_lines.
[[nodiscard]] inline std::vector<std::string> read_lines(const std::filesystem::path& path) {
    return split_lines(read_file(path));
}

// Sets an environment variable for the lifetime of the object and puts the
// previous value (or its absence) back afterwards. The tests run on one
// thread; nothing else reads the environment meanwhile.
class ScopedEnvironment {
public:
    // `value` nullopt: the variable is removed.
    ScopedEnvironment(std::string name, const std::optional<std::string>& value)
        : name_(std::move(name)), previous_(get_(name_)) {
        set_(name_, value);
    }
    ~ScopedEnvironment() { set_(name_, previous_); }
    ScopedEnvironment(const ScopedEnvironment&) = delete;
    ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;
    ScopedEnvironment(ScopedEnvironment&&) = delete;
    ScopedEnvironment& operator=(ScopedEnvironment&&) = delete;

private:
    [[nodiscard]] static std::optional<std::string> get_(const std::string& name) {
#ifdef PLATFORM_WINDOWS
        // MSVC rejects std::getenv under warnings-as-errors; _dupenv_s hands
        // out a copy made with malloc.
        char* copy = nullptr;  // owned by `value` from the next statement on
        std::size_t length = 0;
        const errno_t failed = _dupenv_s(&copy, &length, name.c_str());
        const std::unique_ptr<char, decltype(&std::free)> value(copy, &std::free);
        if (failed != 0 || value == nullptr) {
            return std::nullopt;
        }
        return std::string(value.get());
#else
        const char* value = std::getenv(name.c_str());  // NOLINT(concurrency-mt-unsafe): see above
        return value == nullptr ? std::nullopt : std::optional<std::string>(value);
#endif
    }
    static void set_(const std::string& name, const std::optional<std::string>& value) {
#ifdef PLATFORM_WINDOWS
        EXPECT_EQ(_putenv_s(name.c_str(), value ? value->c_str() : ""), 0) << name;
#else
        const int result = value ? setenv(name.c_str(), value->c_str(), 1)
                                 : unsetenv(name.c_str());  // NOLINT(concurrency-mt-unsafe)
        EXPECT_EQ(result, 0) << name;
#endif
    }

    std::string name_;
    std::optional<std::string> previous_;
};

// Sets TZ, and has the C library read it again, for the object's lifetime.
class ScopedTimeZone {
public:
    explicit ScopedTimeZone(const std::string& zone)
        : variable_(std::make_unique<ScopedEnvironment>("TZ", zone)) {
        reread();
    }
    ~ScopedTimeZone() {
        variable_.reset();
        reread();
    }
    ScopedTimeZone(const ScopedTimeZone&) = delete;
    ScopedTimeZone& operator=(const ScopedTimeZone&) = delete;
    ScopedTimeZone(ScopedTimeZone&&) = delete;
    ScopedTimeZone& operator=(ScopedTimeZone&&) = delete;

private:
    static void reread() {
#ifdef PLATFORM_WINDOWS
        _tzset();
#else
        tzset();  // NOLINT(concurrency-mt-unsafe): the tests run on one thread
#endif
    }

    std::unique_ptr<ScopedEnvironment> variable_;
};

#ifndef PLATFORM_WINDOWS
// Puts a pseudo-terminal of the given size in place of standard error for the
// lifetime of the object, and gives back what was written to it. Standard
// error is restored afterwards.
class StandardErrorOnPty {
public:
    StandardErrorOnPty(uint16_t columns, uint16_t rows) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open(2) is variadic
        master_ = posix_openpt(O_RDWR | O_NOCTTY);
        if (master_ < 0 || grantpt(master_) != 0 || unlockpt(master_) != 0) {
            ADD_FAILURE() << "no pseudo-terminal";
            return;
        }
        const char* name = ptsname(master_);  // NOLINT(concurrency-mt-unsafe): one thread
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open(2) is variadic
        const int slave = name == nullptr ? -1 : open(name, O_RDWR | O_NOCTTY);
        if (slave < 0) {
            ADD_FAILURE() << "cannot open the pseudo-terminal";
            return;
        }
        // Raw output: the bytes read back are the bytes written ("\n" stays
        // "\n" instead of becoming "\r\n").
        termios settings{};
        EXPECT_EQ(tcgetattr(slave, &settings), 0);
        cfmakeraw(&settings);
        EXPECT_EQ(tcsetattr(slave, TCSANOW, &settings), 0);
        winsize size{};
        size.ws_col = columns;
        size.ws_row = rows;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): ioctl(2) is variadic
        EXPECT_EQ(ioctl(slave, TIOCSWINSZ, &size), 0);
        std::cerr.flush();
        saved_ = dup(STDERR_FILENO);
        EXPECT_EQ(dup2(slave, STDERR_FILENO), STDERR_FILENO);
        EXPECT_EQ(close(slave), 0);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): fcntl(2) is variadic
        EXPECT_EQ(fcntl(master_, F_SETFL, O_NONBLOCK), 0);
    }
    ~StandardErrorOnPty() {
        std::cerr.flush();
        if (saved_ >= 0) {
            EXPECT_EQ(dup2(saved_, STDERR_FILENO), STDERR_FILENO);
            EXPECT_EQ(close(saved_), 0);
        }
        if (master_ >= 0) {
            EXPECT_EQ(close(master_), 0);
        }
    }
    StandardErrorOnPty(const StandardErrorOnPty&) = delete;
    StandardErrorOnPty& operator=(const StandardErrorOnPty&) = delete;
    StandardErrorOnPty(StandardErrorOnPty&&) = delete;
    StandardErrorOnPty& operator=(StandardErrorOnPty&&) = delete;

    // What was written to standard error since the last call (and has
    // reached the pty).
    [[nodiscard]] std::string take_output() {
        std::string text;
        std::array<char, 4096> buffer{};
        while (master_ >= 0) {
            const ssize_t got = read(master_, buffer.data(), buffer.size());
            if (got <= 0) {
                break;  // EAGAIN: nothing more for now
            }
            text.append(buffer.data(), static_cast<std::size_t>(got));
        }
        return text;
    }

private:
    int master_ = -1;
    int saved_ = -1;
};

// Puts /dev/null, which is not a terminal, in place of standard error for the
// lifetime of the object.
class StandardErrorToFile {
public:
    StandardErrorToFile() {
        std::cerr.flush();
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open(2) is variadic
        const int null = open("/dev/null", O_WRONLY);
        EXPECT_GE(null, 0);
        saved_ = dup(STDERR_FILENO);
        EXPECT_EQ(dup2(null, STDERR_FILENO), STDERR_FILENO);
        EXPECT_EQ(close(null), 0);
    }
    ~StandardErrorToFile() {
        std::cerr.flush();
        EXPECT_EQ(dup2(saved_, STDERR_FILENO), STDERR_FILENO);
        EXPECT_EQ(close(saved_), 0);
    }
    StandardErrorToFile(const StandardErrorToFile&) = delete;
    StandardErrorToFile& operator=(const StandardErrorToFile&) = delete;
    StandardErrorToFile(StandardErrorToFile&&) = delete;
    StandardErrorToFile& operator=(StandardErrorToFile&&) = delete;

private:
    int saved_ = -1;
};
#endif

// A stream buffer that fails every write.
class FailingBuffer : public std::streambuf {
protected:
    int_type overflow(int_type /*ch*/) override { return traits_type::eof(); }
    std::streamsize xsputn(const char_type* /*s*/, std::streamsize /*n*/) override { return 0; }
};

// A stream buffer whose first write fails; later writes are collected. Read
// text() only after the writer is done (a sink's flush() has returned).
class FailOnceBuffer : public std::streambuf {
public:
    [[nodiscard]] const std::string& text() const { return text_; }

protected:
    std::streamsize xsputn(const char_type* s, std::streamsize n) override {
        if (!failed_) {
            failed_ = true;
            return 0;
        }
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
    bool failed_ = false;
    std::string text_;
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
