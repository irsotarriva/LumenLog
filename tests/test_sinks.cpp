#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "lumen/core.h"
#include "lumen/predicate.h"
#include "lumen/sink.h"

namespace lumen {
namespace {

void wait_flush(int ms = 100) {
    core().flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

class SinkTest : public ::testing::Test {
protected:
    std::vector<SinkId> __ids;

    void TearDown() override {
        for (auto id : __ids) {
            core().remove_sink(id);
        }
        __ids.clear();
    }

    SinkId register_sink(std::unique_ptr<Sink> sink, Predicate pred) {
        SinkId id = core().add_sink(std::move(sink), std::move(pred));
        __ids.push_back(id);
        return id;
    }
};

TEST_F(SinkTest, NullSinkDoesNotThrow) {
    auto sink = std::make_unique<NullSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), always());

    LOG_INFO("to null");
    EXPECT_NO_THROW(wait_flush());
}

TEST_F(SinkTest, TerminalSinkDoesNotThrow) {
    auto sink = std::make_unique<TerminalSink>(TerminalSink::default_config());
    register_sink(std::move(sink), always());

    LOG_INFO("to terminal");
    EXPECT_NO_THROW(wait_flush());
}

TEST_F(SinkTest, TerminalSinkDefaultConfig) {
    auto cfg = TerminalSink::default_config();
    for (size_t i = 1; i < 6; ++i) {
        EXPECT_NE(cfg.colors[i], AnsiColor::Default)
            << "Level " << i << " should have a default color";
    }
    EXPECT_EQ(cfg.colors[0], AnsiColor::Default)
        << "TRACE should be default (dim)";
}

TEST_F(SinkTest, FileSinkWritesToDisk) {
    std::string path = "test_file_sink_output.log";
    std::remove(path.c_str());

    {
        auto cfg = FileSink::Config{};
        cfg.path = path;
        auto sink = std::make_unique<FileSink>(cfg);
        SinkId id = register_sink(std::move(sink), always());

        LOG_INFO("file message one").tag("test", "file");
        wait_flush();
        core().remove_sink(id);
    }

    std::ifstream infile(path);
    ASSERT_TRUE(infile.is_open())
        << "File should be created at " << path;
    std::string content;
    std::getline(infile, content);
    EXPECT_NE(content.find("file message one"), std::string::npos);
    EXPECT_NE(content.find("test=file"), std::string::npos);
    infile.close();
    std::remove(path.c_str());
}

TEST_F(SinkTest, JsonSinkValidNdjson) {
    std::string path = "test_json_sink_output.ndjson";
    std::remove(path.c_str());

    {
        auto sink = std::make_unique<JsonSink>(path);
        SinkId id = register_sink(std::move(sink), always());

        LOG_INFO("json message").tag("env", "test");
        wait_flush();
        core().remove_sink(id);
    }

    std::ifstream infile(path);
    ASSERT_TRUE(infile.is_open())
        << "NDJSON file should exist at " << path;
    std::string line;
    std::getline(infile, line);
    EXPECT_NE(line.find("\"type\":\"log\""), std::string::npos);
    EXPECT_NE(line.find("\"msg\":\"json message\""), std::string::npos);
    EXPECT_NE(line.find("\"env\":\"test\""), std::string::npos);
    infile.close();
    std::remove(path.c_str());
}

TEST_F(SinkTest, FlushCompletesPendingWrites) {
    std::string path = "test_flush_output.log";
    std::remove(path.c_str());

    auto cfg = FileSink::Config{};
    cfg.path = path;
    auto sink = std::make_unique<FileSink>(cfg);
    auto* raw = sink.get();
    SinkId id = register_sink(std::move(sink), always());

    LOG_INFO("before flush");
    wait_flush(50);

    raw->flush();

    std::ifstream infile(path);
    ASSERT_TRUE(infile.is_open());
    std::string content;
    std::getline(infile, content);
    EXPECT_NE(content.find("before flush"), std::string::npos);
    infile.close();
    std::remove(path.c_str());
}

TEST_F(SinkTest, FileSinkHandlesMultipleRecords) {
    std::string path = "test_multi_output.log";
    std::remove(path.c_str());

    auto cfg = FileSink::Config{};
    cfg.path = path;
    {
        auto sink = std::make_unique<FileSink>(cfg);
        SinkId id = register_sink(std::move(sink), always());

        LOG_INFO("first");
        LOG_INFO("second");
        LOG_INFO("third");
        wait_flush();
        core().remove_sink(id);
    }

    std::ifstream infile(path);
    ASSERT_TRUE(infile.is_open());
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(infile, line)) {
        lines.push_back(line);
    }

    EXPECT_GE(lines.size(), 3);
    bool found_first = false;
    bool found_second = false;
    bool found_third = false;
    for (const auto& l : lines) {
        if (l.find("first") != std::string::npos) found_first = true;
        if (l.find("second") != std::string::npos) found_second = true;
        if (l.find("third") != std::string::npos) found_third = true;
    }
    EXPECT_TRUE(found_first);
    EXPECT_TRUE(found_second);
    EXPECT_TRUE(found_third);
    infile.close();
    std::remove(path.c_str());
}

// ── Error-path tests ──────────────────────────────────────────────────────────

TEST_F(SinkTest, FileSinkHandlesMissingDirectoryGracefully) {
    std::string path = "/nonexistent/path/test_output.log";

    auto cfg = FileSink::Config{};
    cfg.path = path;
    auto sink = std::make_unique<FileSink>(cfg);

    LOG_INFO("message to broken sink");

    EXPECT_NO_THROW(wait_flush());
    sink->flush();
}

TEST_F(SinkTest, JsonSinkHandlesRapidRecords) {
    std::string path = "test_json_rapid.ndjson";
    std::remove(path.c_str());

    auto sink = std::make_unique<JsonSink>(path);
    SinkId id = register_sink(std::move(sink), always());

    for (int i = 0; i < 50; ++i) {
        LOG_INFO("rapid message").tag("idx", static_cast<int64_t>(i));
    }

    wait_flush();
    core().remove_sink(id);
    {
        std::ifstream infile(path);
        ASSERT_TRUE(infile.is_open());
        int lines = 0;
        std::string line;
        while (std::getline(infile, line)) {
            ++lines;
        }
        EXPECT_GE(lines, 1);
    }
    std::remove(path.c_str());
}

TEST_F(SinkTest, TerminalSinkWithEmptyConfig) {
    TerminalSink::Config cfg{};
    auto sink = std::make_unique<TerminalSink>(cfg);
    register_sink(std::move(sink), always());

    LOG_INFO("empty config test");

    EXPECT_NO_THROW(wait_flush());
}

TEST_F(SinkTest, FlushWithNoRecordsDoesNotBlock) {
    auto sink = std::make_unique<NullSink>();
    auto id = register_sink(std::move(sink), always());

    auto t0 = std::chrono::steady_clock::now();
    EXPECT_NO_THROW(wait_flush(10));
    auto t1 = std::chrono::steady_clock::now();

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0);
    EXPECT_LT(elapsed.count(), 500);
}

TEST_F(SinkTest, JsonSinkHandlesSpecialCharacters) {
    std::string path = "test_json_special.ndjson";
    std::remove(path.c_str());

    auto sink = std::make_unique<JsonSink>(path);
    SinkId id = register_sink(std::move(sink), always());

    LOG_INFO("message with \"quotes\" and \\backslash").tag("special", "value\nwith\rnewline");

    wait_flush();
    core().remove_sink(id);

    std::ifstream infile(path);
    ASSERT_TRUE(infile.is_open());
    std::string content;
    std::getline(infile, content);
    EXPECT_NE(content.find("\"msg\":\"message with \\\"quotes\\\" and \\\\backslash\""),
              std::string::npos);
    infile.close();
    std::remove(path.c_str());
}

}  // namespace
}  // namespace lumen
