// Rotation by size (src/rotating_file.cpp): through FileSink for the
// documented layout of the files, and through RotatingFile directly for the
// failure paths, whose effects are in one batch's outcome.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "lumen/error.hpp"
#include "lumen/file_sink.hpp"
#include "rotating_file.hpp"
#include "sink_test_support.hpp"

namespace lumen {
namespace {

using test::log_record;

class RotationTest : public ::testing::Test {
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

    // A RotatingFile on `name`; diagnostics of a failed open are discarded.
    [[nodiscard]] std::unique_ptr<detail::RotatingFile> _file(
        const std::string& name, detail::RotatingFile::Limits limits) const {
        std::expected<detail::RotatingFile, std::error_code> file =
            detail::RotatingFile::open(_path(name), limits, {});
        EXPECT_TRUE(file.has_value());
        return file ? std::make_unique<detail::RotatingFile>(std::move(*file)) : nullptr;
    }

    // A directory that holds a file, so that it can be neither removed nor
    // replaced by a rename.
    void _make_obstacle(const std::string& name) const {
        std::filesystem::create_directory(_path(name));
        std::ofstream(_path(name) / "keep") << "x";
    }

private:
    test::TempDir dir_;
};

using Messages = std::vector<std::string>;

// Ten bytes each.
[[nodiscard]] std::vector<std::string> ten_byte_lines(uint32_t count) {
    std::vector<std::string> lines;
    for (uint32_t i = 0; i < count; ++i) {
        lines.push_back("line " + std::to_string(1000 + i) + "\n");
    }
    return lines;
}

[[nodiscard]] std::size_t occurrences(std::string_view text, std::string_view part) {
    std::size_t count = 0;
    for (std::size_t pos = text.find(part); pos != std::string_view::npos;
         pos = text.find(part, pos + part.size())) {
        ++count;
    }
    return count;
}

// Two lines fit in a file. Seven lines with max_files = 3: the active file
// and two old ones survive; the first two lines are gone with the deleted
// oldest file.
TEST_F(RotationTest, RotationKeepsMaxFilesFilesNoneLargerThanTheLimit) {
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

TEST_F(RotationTest, RotationCountsTheSizeTheFileHadWhenTheSinkWasCreated) {
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

TEST_F(RotationTest, WithOneFileRotationStartsTheFileAgain) {
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

TEST_F(RotationTest, ZeroByteLimitNeverRotates) {
    FileSink::Config config = _config("out.log");
    config.max_file_bytes = 0;
    const auto sink = _sink(std::move(config));
    ASSERT_NE(sink, nullptr);
    _log_numbered(*sink, 0, 3);
    EXPECT_EQ(_messages(_path("out.log")), (Messages{"m0", "m1", "m2"}));
    EXPECT_FALSE(std::filesystem::exists(_path("out.log.1")));
}

TEST_F(RotationTest, LineLongerThanTheLimitIsWrittenWholeIntoAnEmptyFile) {
    FileSink::Config config = _config("out.log");
    config.max_file_bytes = 10;
    const auto sink = _sink(std::move(config));
    ASSERT_NE(sink, nullptr);
    _log_numbered(*sink, 0, 2);
    EXPECT_EQ(_messages(_path("out.log")), (Messages{"m1"}));
    EXPECT_EQ(_messages(_path("out.log.1")), (Messages{"m0"}));
    EXPECT_FALSE(std::filesystem::exists(_path("out.log.2")));  // the empty file was not rotated
}

// out.log.2 cannot be removed and out.log.1 cannot be renamed onto it; the
// active file still moves to out.log.1 (replacing the old one). Both failed
// steps are in the outcome, no line is lost.
TEST_F(RotationTest, FailedRemoveAndRenameOfOldFilesAreBothReportedAndNoLineIsLost) {
    _make_obstacle("out.log.2");
    std::ofstream(_path("out.log.1")) << "old\n";
    const auto file = _file("out.log", {.max_file_bytes = 20, .max_files = 3});
    ASSERT_NE(file, nullptr);
    const detail::BatchOutcome outcome = file->write(ten_byte_lines(3));
    EXPECT_EQ(outcome.failed_lines, 0U);
    EXPECT_NE(outcome.failure.find("cannot remove " + _path("out.log.2").string() + ": "),
              std::string::npos)
        << outcome.failure;
    EXPECT_NE(outcome.failure.find("cannot rename " + _path("out.log.1").string() + ": "),
              std::string::npos)
        << outcome.failure;
    EXPECT_EQ(test::read_file(_path("out.log.1")), "line 1000\nline 1001\n");
    EXPECT_EQ(test::read_file(_path("out.log")), "line 1002\n");
}

// The active file cannot be moved away (out.log.1 is in the way), so it
// keeps growing; a new attempt is made only after another max_file_bytes:
// at 20 and 40 bytes, not before each of the four lines from 20 bytes on.
TEST_F(RotationTest, FailedRotationIsRetriedOnlyAfterTheFileGrewByAnotherLimit) {
    _make_obstacle("out.log.1");
    const auto file = _file("out.log", {.max_file_bytes = 20, .max_files = 2});
    ASSERT_NE(file, nullptr);
    const detail::BatchOutcome outcome = file->write(ten_byte_lines(6));
    EXPECT_EQ(outcome.failed_lines, 0U);
    EXPECT_EQ(occurrences(outcome.failure, "cannot rename " + _path("out.log").string() + ": "), 2U)
        << outcome.failure;
    EXPECT_EQ(std::filesystem::file_size(_path("out.log")), 60U);
}

// The attempt at 20 bytes fails, so the next one is due past 40 bytes. Once
// the obstacle is gone, that attempt succeeds, and the limit counts from the
// new, empty file again.
TEST_F(RotationTest, RotationWorksAgainOnceTheObstacleIsGone) {
    _make_obstacle("out.log.1");
    const auto file = _file("out.log", {.max_file_bytes = 20, .max_files = 2});
    ASSERT_NE(file, nullptr);
    EXPECT_FALSE(file->write(ten_byte_lines(3)).failure.empty());  // 30 bytes, not moved
    std::filesystem::remove_all(_path("out.log.1"));
    // 1000 takes the file to 40 bytes; 1001 rotates first (the 40 bytes go
    // to out.log.1); 1003 rotates again at 20 bytes, the configured limit.
    const detail::BatchOutcome outcome = file->write(ten_byte_lines(4));
    EXPECT_EQ(outcome.failure, "");
    EXPECT_EQ(test::read_file(_path("out.log.1")), "line 1001\nline 1002\n");
    EXPECT_EQ(test::read_file(_path("out.log")), "line 1003\n");
}

// Each open failure gives LumenError::FileOpenFailed, and one diagnostic line
// with the cause the platform reported.
TEST_F(RotationTest, OpenFailureReportsItsCauseThroughTheDiagnosticHandler) {
    std::filesystem::create_directory(_path("a_directory"));
    for (const auto& [name, cause] :
         {std::pair<std::string, std::errc>{"a_directory", std::errc::is_a_directory},
          std::pair<std::string, std::errc>{"no_such_dir/out.log",
                                            std::errc::no_such_file_or_directory}}) {
        std::vector<std::string> reported;
        const std::expected<detail::RotatingFile, std::error_code> file =
            detail::RotatingFile::open(_path(name), {},
                                       [&](std::string_view line) { reported.emplace_back(line); });
        ASSERT_FALSE(file.has_value()) << name;
        EXPECT_EQ(file.error(), LumenError::FileOpenFailed) << name;
        ASSERT_EQ(reported.size(), 1U) << name;
        EXPECT_EQ(reported.front(), "lumen: FileSink: cannot open " + _path(name).string() + ": " +
                                        std::make_error_code(cause).message());
    }
}

}  // namespace
}  // namespace lumen
