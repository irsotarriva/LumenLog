// FileSink: creation and its errors, the {date} placeholder, appending,
// rotation, and write-error accounting. The line formats themselves are
// checked in test_record_format.cpp.
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef PLATFORM_WINDOWS
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "lumen/error.hpp"
#include "lumen/file_sink.hpp"
#include "sink_test_support.hpp"
#include "test_support.hpp"

namespace lumen {
namespace {

using test::log_record;

// YYYY-MM-DD of the current UTC date, computed independently of the sink.
[[nodiscard]] std::string utc_date_now() {
    const auto today = std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now());
    const std::chrono::year_month_day ymd{today};
    return std::format("{:04}-{:02}-{:02}", static_cast<int>(ymd.year()),
                       static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()));
}

class FileSinkTest : public ::testing::Test {
protected:
    [[nodiscard]] std::filesystem::path _path(std::string_view name) const { return dir_ / name; }

    [[nodiscard]] FileSink::Config _config(const std::string& name) const {
        FileSink::Config config;
        config.path = _path(name);
        config.format = FileFormat::Json;
        config.diagnostics = {};
        return config;
    }

    [[nodiscard]] static std::unique_ptr<FileSink> _sink(FileSink::Config config) {
        auto sink = FileSink::create(std::move(config));
        EXPECT_TRUE(sink.has_value());
        return sink ? std::move(*sink) : nullptr;
    }

    // Logs the messages "m0", "m1", ... (one Json line each, all the same
    // length) and flushes.
    static void _log_numbered(FileSink& sink, uint32_t first, uint32_t count) {
        for (uint32_t i = first; i < first + count; ++i) {
            const std::string message = "m" + std::to_string(i);
            sink.on_log(log_record(message));
        }
        sink.flush();
    }

    // The messages in a file written by _log_numbered, in order.
    [[nodiscard]] static std::vector<std::string> _messages(const std::filesystem::path& path) {
        std::vector<std::string> out;
        for (const std::string& line : test::read_lines(path)) {
            const std::size_t start = line.find(R"("msg":")") + 7;
            out.push_back(line.substr(start, line.find('"', start) - start));
        }
        return out;
    }

private:
    test::TempDir dir_;
};

using Messages = std::vector<std::string>;

TEST_F(FileSinkTest, CreateOpensTheConfiguredFileAndPathReturnsIt) {
    const auto sink = _sink(_config("out.log"));
    ASSERT_NE(sink, nullptr);
    EXPECT_EQ(sink->path(), _path("out.log"));
    EXPECT_TRUE(std::filesystem::exists(_path("out.log")));
}

TEST_F(FileSinkTest, DatePlaceholderIsReplacedByTheUtcDateAtCreation) {
    const std::string before = utc_date_now();
    const auto sink = _sink(_config("run_{date}_{date}.log"));
    const std::string after = utc_date_now();
    ASSERT_NE(sink, nullptr);
    const std::string name = sink->path().filename().string();
    EXPECT_TRUE(name == "run_" + before + "_" + before + ".log" ||
                name == "run_" + after + "_" + after + ".log")
        << name;
    EXPECT_TRUE(std::filesystem::exists(sink->path()));
    EXPECT_FALSE(std::filesystem::exists(_path("run_{date}_{date}.log")));
}

TEST_F(FileSinkTest, DefaultPathHasTheDatePlaceholder) {
    EXPECT_EQ(FileSink::Config{}.path, std::filesystem::path("run_{date}.log"));
}

TEST_F(FileSinkTest, CreateFailsWithFileOpenFailedForAMissingDirectoryOrADirectory) {
    const auto missing = FileSink::create(_config("no_such_dir/out.log"));
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error(), LumenError::FileOpenFailed);

    std::filesystem::create_directory(_path("a_directory"));
    const auto directory = FileSink::create(_config("a_directory"));
    ASSERT_FALSE(directory.has_value());
    EXPECT_EQ(directory.error(), LumenError::FileOpenFailed);
}

TEST_F(FileSinkTest, CreateRejectsZeroFilesZeroQueueAndAnEmptyPath) {
    FileSink::Config no_files = _config("a.log");
    no_files.max_files = 0;
    FileSink::Config no_queue = _config("b.log");
    no_queue.max_queued_lines = 0;
    FileSink::Config no_path = _config("c.log");
    no_path.path.clear();
    for (FileSink::Config& config : {std::ref(no_files), std::ref(no_queue), std::ref(no_path)}) {
        const auto sink = FileSink::create(std::move(config));
        ASSERT_FALSE(sink.has_value());
        EXPECT_EQ(sink.error(), LumenError::InvalidSinkConfig);
    }
    EXPECT_FALSE(std::filesystem::exists(_path("a.log")));
}

TEST_F(FileSinkTest, ExistingFileIsAppendedToNotTruncated) {
    {
        std::ofstream existing(_path("out.log"), std::ios::binary);
        existing << "earlier line\n";
    }
    const auto sink = _sink(_config("out.log"));
    ASSERT_NE(sink, nullptr);
    _log_numbered(*sink, 0, 1);
    const std::vector<std::string> lines = test::read_lines(_path("out.log"));
    ASSERT_EQ(lines.size(), 2U);
    EXPECT_EQ(lines.front(), "earlier line");
}

TEST_F(FileSinkTest, DestroyingTheSinkWritesTheLinesStillQueued) {
    {
        const auto sink = _sink(_config("out.log"));
        ASSERT_NE(sink, nullptr);
        sink->on_log(log_record("m0"));
        sink->on_log(log_record("m1"));
    }
    EXPECT_EQ(_messages(_path("out.log")), (Messages{"m0", "m1"}));
}

#ifndef PLATFORM_WINDOWS
// A named pipe in the test's directory; `reader` opens its read end.
class NamedPipe {
public:
    NamedPipe(const std::filesystem::path& path, bool reader) {
        EXPECT_EQ(mkfifo(path.c_str(), 0600), 0);
        if (reader) {
            // Non-blocking, so that opening needs no writer and reading never waits.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open(2) is variadic
            reader_ = open(path.c_str(), O_RDONLY | O_NONBLOCK);
            EXPECT_GE(reader_, 0);
        }
    }
    ~NamedPipe() {
        if (reader_ >= 0) {
            EXPECT_EQ(close(reader_), 0);
        }
    }
    NamedPipe(const NamedPipe&) = delete;
    NamedPipe& operator=(const NamedPipe&) = delete;
    NamedPipe(NamedPipe&&) = delete;
    NamedPipe& operator=(NamedPipe&&) = delete;

    [[nodiscard]] std::string read_all() const {
        std::string text;
        std::array<char, 4096> buffer{};
        for (ssize_t got = 0; (got = read(reader_, buffer.data(), buffer.size())) > 0;) {
            text.append(buffer.data(), static_cast<std::size_t>(got));
        }
        return text;
    }

private:
    int reader_ = -1;
};

TEST_F(FileSinkTest, AfterAFatalRecordFlushWaitsForANamedPipeNobodyReadsOnlyUntilTheTimeout) {
    NamedPipe pipe(_path("pipe"), true);
    FileSink::Config config = _config("pipe");
    config.format = FileFormat::Text;
    std::unique_ptr<FileSink> sink = _sink(std::move(config));
    ASSERT_NE(sink, nullptr);
    // Far more than a pipe holds, so the sink's writer blocks in write(2).
    const std::string long_message(1000, 'x');
    for (uint32_t i = 0; i < 1000; ++i) {
        sink->on_log(log_record(long_message));
    }
    sink->on_log(log_record("the end", LogLevel::Fatal));
    const auto start = std::chrono::steady_clock::now();
    std::future<void> flushed = std::async(std::launch::async, [&sink] { sink->flush(); });
    const std::future_status status =
        flushed.wait_for(fatal_flush_timeout + std::chrono::seconds(5));
    const auto waited = std::chrono::steady_clock::now() - start;
    // Unblock the writer whatever happened above, so that the flush (if it
    // is still waiting) and the sink's destructor can end.
    std::atomic<bool> sink_gone{false};
    std::thread drain([&] {
        while (!sink_gone.load()) {
            static_cast<void>(pipe.read_all());  // the content is not checked here
            std::this_thread::yield();
        }
    });
    flushed.get();
    sink.reset();
    sink_gone = true;
    drain.join();
    EXPECT_EQ(status, std::future_status::ready) << "flush waited for the blocked pipe";
    EXPECT_GE(waited, fatal_flush_timeout);
}

TEST_F(FileSinkTest, CreateOnANamedPipeWithoutAReaderFailsAtOnceAndSaysWhy) {
    const NamedPipe pipe(_path("pipe"), false);
    std::vector<std::string> reported;
    FileSink::Config config = _config("pipe");
    config.diagnostics = [&](std::string_view line) { reported.emplace_back(line); };
    const auto sink = FileSink::create(std::move(config));
    ASSERT_FALSE(sink.has_value());
    EXPECT_EQ(sink.error(), LumenError::FileOpenFailed);
    ASSERT_EQ(reported.size(), 1U);
    EXPECT_NE(reported.front().find("named pipe without a reader"), std::string::npos)
        << reported.front();
}

// A size limit far below what is written: a pipe must still never be renamed
// away (as root, rename() would move the special file itself).
TEST_F(FileSinkTest, NamedPipeWithAReaderReceivesTheLinesAndIsNeverRotated) {
    const NamedPipe pipe(_path("pipe"), true);
    FileSink::Config config = _config("pipe");
    config.format = FileFormat::Text;
    config.max_file_bytes = 10;
    const auto sink = _sink(std::move(config));
    ASSERT_NE(sink, nullptr);
    _log_numbered(*sink, 0, 5);
    const std::vector<std::string> lines = test::split_lines(pipe.read_all());
    ASSERT_EQ(lines.size(), 5U);
    EXPECT_NE(lines.at(4).find(" INFO  m4 [t3]"), std::string::npos) << lines.at(4);
    EXPECT_FALSE(std::filesystem::exists(_path("pipe.1")));
    EXPECT_EQ(std::filesystem::status(_path("pipe")).type(), std::filesystem::file_type::fifo);
    EXPECT_EQ(sink->statistics().lines_written, 5U);
}
#endif

// /dev/full accepts the open and fails every write with ENOSPC.
TEST_F(FileSinkTest, WriteErrorsAreCountedPerLineAndTheFirstIsReportedOnce) {
    if (!std::filesystem::exists("/dev/full")) {
        GTEST_SKIP() << "needs /dev/full (Linux)";
    }
    std::mutex mutex;
    std::vector<std::string> reported;  // guarded by mutex
    FileSink::Config config;
    config.path = "/dev/full";
    config.diagnostics = [&](std::string_view text) {
        const std::scoped_lock lock(mutex);
        reported.emplace_back(text);
    };
    const auto sink = _sink(std::move(config));
    ASSERT_NE(sink, nullptr);
    for (uint32_t i = 0; i < 3; ++i) {
        sink->on_log(log_record("lost"));
        sink->flush();
    }
    const WriterStatistics stats = sink->statistics();
    EXPECT_EQ(stats.lines_queued, 3U);
    EXPECT_EQ(stats.lines_failed, 3U);
    EXPECT_EQ(stats.lines_written, 0U);
    const std::scoped_lock lock(mutex);
    ASSERT_EQ(reported.size(), 1U);
    EXPECT_NE(reported.front().find("FileSink: write failed (cannot write /dev/full"),
              std::string::npos)
        << reported.front();
}

class FileSinkOnCoreTest : public test::LocalCoreTest {};

TEST_F(FileSinkOnCoreTest, RecordsLoggedThroughACoreAreInTheFileWhenFlushReturns) {
    const test::TempDir dir;
    auto sink = FileSink::create({.path = dir / "out.log", .format = FileFormat::Text});
    ASSERT_TRUE(sink.has_value());
    ASSERT_TRUE(_core.add_sink(std::move(*sink)).has_value());
    test::log_to(_core, "first").tag("k", "v");
    test::log_to(_core, "second");
    _core.flush();
    const std::vector<std::string> lines = test::read_lines(dir / "out.log");
    ASSERT_EQ(lines.size(), 2U);
    EXPECT_NE(lines.at(0).find(" INFO  first k=v [t"), std::string::npos) << lines.at(0);
    EXPECT_NE(lines.at(1).find(" INFO  second [t"), std::string::npos) << lines.at(1);
}

}  // namespace
}  // namespace lumen
