// FileSink: creation and its errors, the {date} placeholder, appending,
// rotation, and write-error accounting. The line formats themselves are
// checked in test_record_format.cpp.
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

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

    // The bytes of one line written by _log_numbered.
    [[nodiscard]] uint64_t _line_bytes() const {
        const auto probe = _sink(_config("probe.log"));
        if (!probe) {
            return 0;
        }
        _log_numbered(*probe, 0, 1);
        return std::filesystem::file_size(probe->path());
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

// Two lines fit in a file. Seven lines with max_files = 3: the active file
// and two old ones survive; the first two lines are gone with the deleted
// oldest file.
TEST_F(FileSinkTest, RotationKeepsMaxFilesFilesNoneLargerThanTheLimit) {
    const uint64_t line = _line_bytes();
    ASSERT_GT(line, 0U);
    FileSink::Config config = _config("out.log");
    config.max_file_bytes = 2 * line;
    config.max_files = 3;
    const auto sink = _sink(std::move(config));
    ASSERT_NE(sink, nullptr);
    _log_numbered(*sink, 0, 7);

    EXPECT_EQ(_messages(_path("out.log")), (Messages{"m6"}));
    EXPECT_EQ(_messages(_path("out.log.1")), (Messages{"m4", "m5"}));
    EXPECT_EQ(_messages(_path("out.log.2")), (Messages{"m2", "m3"}));
    EXPECT_FALSE(std::filesystem::exists(_path("out.log.3")));
    EXPECT_EQ(sink->statistics().lines_written, 7U);
    EXPECT_EQ(sink->statistics().lines_failed, 0U);
}

TEST_F(FileSinkTest, RotationCountsTheSizeTheFileHadWhenTheSinkWasCreated) {
    const uint64_t line = _line_bytes();
    ASSERT_GT(line, 0U);
    FileSink::Config config = _config("out.log");
    config.max_file_bytes = 2 * line;
    config.max_files = 2;
    {
        const auto first_run = _sink(config);
        ASSERT_NE(first_run, nullptr);
        _log_numbered(*first_run, 0, 1);
    }
    const auto second_run = _sink(config);
    ASSERT_NE(second_run, nullptr);
    _log_numbered(*second_run, 1, 2);  // m1 fills the file, m2 starts a new one

    EXPECT_EQ(_messages(_path("out.log.1")), (Messages{"m0", "m1"}));
    EXPECT_EQ(_messages(_path("out.log")), (Messages{"m2"}));
}

TEST_F(FileSinkTest, WithOneFileRotationStartsTheFileAgain) {
    const uint64_t line = _line_bytes();
    FileSink::Config config = _config("out.log");
    config.max_file_bytes = line;
    config.max_files = 1;
    const auto sink = _sink(std::move(config));
    ASSERT_NE(sink, nullptr);
    _log_numbered(*sink, 0, 3);
    EXPECT_EQ(_messages(_path("out.log")), (Messages{"m2"}));
    EXPECT_FALSE(std::filesystem::exists(_path("out.log.1")));
}

TEST_F(FileSinkTest, ZeroByteLimitNeverRotates) {
    FileSink::Config config = _config("out.log");
    config.max_file_bytes = 0;
    const auto sink = _sink(std::move(config));
    ASSERT_NE(sink, nullptr);
    _log_numbered(*sink, 0, 3);
    EXPECT_EQ(_messages(_path("out.log")), (Messages{"m0", "m1", "m2"}));
    EXPECT_FALSE(std::filesystem::exists(_path("out.log.1")));
}

TEST_F(FileSinkTest, LineLongerThanTheLimitIsWrittenWholeIntoAnEmptyFile) {
    FileSink::Config config = _config("out.log");
    config.max_file_bytes = 10;
    const auto sink = _sink(std::move(config));
    ASSERT_NE(sink, nullptr);
    _log_numbered(*sink, 0, 2);
    EXPECT_EQ(_messages(_path("out.log")), (Messages{"m1"}));
    EXPECT_EQ(_messages(_path("out.log.1")), (Messages{"m0"}));
}

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
    for (int i = 0; i < 3; ++i) {
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
