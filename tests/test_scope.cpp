#include <atomic>
#include <chrono>
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

struct CaptureSink : public Sink {
    std::vector<LogRecord> logs;
    std::mutex mtx;

    void on_log(const LogRecord& r) override {
        std::lock_guard lock(mtx);
        logs.push_back(r);
    }
    void on_metric(const MetricRecord&) override {}
    void on_progress(const ProgressRecord&) override {}
    void flush() override {}
};

struct ProgressCaptureSink : public Sink {
    std::vector<ProgressRecord> progress;
    std::mutex mtx;

    void on_log(const LogRecord&) override {}
    void on_metric(const MetricRecord&) override {}
    void on_progress(const ProgressRecord& r) override {
        std::lock_guard lock(mtx);
        progress.push_back(r);
    }
    void flush() override {}
};

void wait_flush(int ms = 100) {
    core().flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

class ScopeTest : public ::testing::Test {
protected:
    std::vector<SinkId> __ids;

    void TearDown() override {
        for (auto id : __ids) {
            (void)core().remove_sink(id);
        }
        __ids.clear();
    }

    SinkId register_sink(std::unique_ptr<Sink> sink, Predicate pred) {
        SinkId id = core().add_sink(std::move(sink), std::move(pred));
        __ids.push_back(id);
        return id;
    }
};

TEST_F(ScopeTest, ScopeTagsPresent) {
    auto sink = std::make_unique<CaptureSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), always());

    {
        LUMEN_SCOPE("scope_key", "scope_val");
        LOG_INFO("inside scope");
    }

    wait_flush();

    ASSERT_GE(raw->logs.size(), 1);

    bool found = false;
    for (auto it = raw->logs[0].tags.begin(); it != raw->logs[0].tags.end(); ++it) {
        if (it->key == "scope_key" && it->value == "scope_val") {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(ScopeTest, ScopeTagsAbsentOutside) {
    auto sink = std::make_unique<CaptureSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), always());

    {
        LUMEN_SCOPE("transient", "present");
        LOG_INFO("inside");
    }

    LOG_INFO("outside");

    wait_flush();

    ASSERT_GE(raw->logs.size(), 2);

    const auto& inside_log = raw->logs[0];
    bool inside_has_tag = false;
    for (auto it = inside_log.tags.begin(); it != inside_log.tags.end(); ++it) {
        if (it->key == "transient") {
            inside_has_tag = true;
            break;
        }
    }
    EXPECT_TRUE(inside_has_tag);

    const auto& outside_log = raw->logs[1];
    bool outside_has_tag = false;
    for (auto it = outside_log.tags.begin(); it != outside_log.tags.end(); ++it) {
        if (it->key == "transient") {
            outside_has_tag = true;
            break;
        }
    }
    EXPECT_FALSE(outside_has_tag);
}

TEST_F(ScopeTest, ScopeRaiiOnException) {
    auto sink = std::make_unique<CaptureSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), always());

    try {
        LUMEN_SCOPE("exception_scope", "active");
        LOG_INFO("before throw");
        throw std::runtime_error("test exception");
    } catch (const std::exception&) {
        LOG_INFO("in catch");
    }

    wait_flush();

    ASSERT_GE(raw->logs.size(), 2);

    const auto& catch_log = raw->logs[1];
    bool catch_has_scope_tag = false;
    for (auto it = catch_log.tags.begin(); it != catch_log.tags.end(); ++it) {
        if (it->key == "exception_scope") {
            catch_has_scope_tag = true;
            break;
        }
    }
    EXPECT_FALSE(catch_has_scope_tag)
        << "Scope tag should not persist after exception";
}

TEST_F(ScopeTest, NestedScopesMergeCorrectly) {
    auto sink = std::make_unique<CaptureSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), always());

    {
        LUMEN_SCOPE("layer", "outer");
        {
            LUMEN_SCOPE("layer", "inner");
            LOG_INFO("inside nested");
        }
    }

    wait_flush();

    ASSERT_GE(raw->logs.size(), 1);
    EXPECT_EQ(raw->logs[0].tags.find("layer"), "inner")
        << "Inner scope tag should override outer";
}

TEST_F(ScopeTest, LoopEmitsProgress) {
    auto sink = std::make_unique<ProgressCaptureSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), always());

    {
        LUMEN_LOOP("test_loop", i, 3) {
            (void)i;
        }
    }

    wait_flush();

    EXPECT_GE(raw->progress.size(), 3)
        << "Should emit at least 3 progress records for 3 iterations";
}

TEST_F(ScopeTest, LoopTagsAttached) {
    auto sink = std::make_unique<CaptureSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), always());

    {
        LUMEN_LOOP("my_loop", i, 2) {
            LOG_INFO("iteration log");
            (void)i;
        }
    }

    wait_flush();

    ASSERT_GE(raw->logs.size(), 1);
    bool has_loop_label = false;
    for (auto it = raw->logs[0].tags.begin(); it != raw->logs[0].tags.end(); ++it) {
        if (it->key == "loop_label") {
            has_loop_label = true;
            EXPECT_EQ(it->value, "my_loop");
        }
    }
    EXPECT_TRUE(has_loop_label);
}

// ── Error-path tests ──────────────────────────────────────────────────────────

TEST_F(ScopeTest, EmptyScopeDoesNotCrash) {
    auto sink = std::make_unique<CaptureSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), always());

    {
        LUMEN_SCOPE("", "");
        LOG_INFO("empty scope");
    }

    wait_flush();
    EXPECT_GE(raw->logs.size(), 1);
}

TEST_F(ScopeTest, MultipleScopesInSameBlock) {
    auto sink = std::make_unique<CaptureSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), always());

    {
        LUMEN_SCOPE("a", "1");
        LUMEN_SCOPE("b", "2");
        LOG_INFO("double scope");
    }

    wait_flush();

    ASSERT_GE(raw->logs.size(), 1);
    EXPECT_EQ(raw->logs[0].tags.find("a"), "1");
    EXPECT_EQ(raw->logs[0].tags.find("b"), "2");
}

TEST_F(ScopeTest, ScopeAfterPrematureScopeExit) {
    auto sink = std::make_unique<CaptureSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), always());

    {
        {
            LUMEN_SCOPE("inner", "val");
        }
        LOG_INFO("after inner scope");
    }

    wait_flush();

    ASSERT_GE(raw->logs.size(), 1);
    EXPECT_TRUE(raw->logs[0].tags.find("inner").empty());
}

TEST_F(ScopeTest, LoopWithZeroIterations) {
    auto sink = std::make_unique<ProgressCaptureSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), always());

    {
        LUMEN_LOOP("empty_loop", i, 0) {
            (void)i;
            FAIL() << "Should not enter loop with zero iterations";
        }
    }

    wait_flush();
    SUCCEED();
}

TEST_F(ScopeTest, LoopWithOneIteration) {
    auto sink = std::make_unique<ProgressCaptureSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), always());

    {
        LUMEN_LOOP("single_loop", i, 1) {
            LOG_INFO("single iteration");
            (void)i;
        }
    }

    wait_flush();
    EXPECT_GE(raw->progress.size(), 1);
}

}  // namespace
}  // namespace lumen
