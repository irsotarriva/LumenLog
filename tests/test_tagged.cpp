#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "lumen/core.h"
#include "lumen/predicate.h"
#include "lumen/sink.h"
#include "lumen/tagged.h"

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

void wait_flush(int ms = 100) {
    core().flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

class TaggedTest : public ::testing::Test {
protected:
    std::vector<SinkId> ids_;

    void TearDown() override {
        for (auto id : ids_) {
            (void)core().remove_sink(id);
        }
        ids_.clear();
    }

    SinkId register_sink(std::unique_ptr<Sink> sink, Predicate pred) {
        SinkId id = core().add_sink(std::move(sink), std::move(pred));
        ids_.push_back(id);
        return id;
    }
};

class MyComponent : public Tagged {
public:
    MyComponent() {
        lumen_tag("component", "test_comp");
        lumen_tag("version", "1.0");
    }

    void do_work() {
        LUMEN_MEMBER_SCOPE;
        LOG_INFO("work from component");
    }

    void do_work_without_scope() {
        LOG_INFO("work without member scope");
    }
};

TEST_F(TaggedTest, InstanceTagsOnMethod) {
    auto sink = std::make_unique<CaptureSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), always());

    MyComponent comp;
    comp.do_work();

    wait_flush();

    ASSERT_GE(raw->logs.size(), 1);
    EXPECT_EQ(raw->logs[0].message, "work from component");
    EXPECT_EQ(raw->logs[0].tags.find("component"), "test_comp");
    EXPECT_EQ(raw->logs[0].tags.find("version"), "1.0");
}

TEST_F(TaggedTest, InstanceTagsAbsentWithout) {
    auto sink = std::make_unique<CaptureSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), always());

    MyComponent comp;
    comp.do_work_without_scope();

    wait_flush();

    ASSERT_GE(raw->logs.size(), 1);
    EXPECT_EQ(raw->logs[0].message, "work without member scope");
    EXPECT_TRUE(raw->logs[0].tags.find("component").empty())
        << "Instance tags should be absent without LUMEN_MEMBER_SCOPE";
}

TEST_F(TaggedTest, ExplicitTagOverridesInstanceTag) {
    auto sink = std::make_unique<CaptureSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), always());

    MyComponent comp;
    {
        auto scope = comp.lumen_scope();
        LOG_INFO("overridden").tag("component", "override");
    }

    wait_flush();

    ASSERT_GE(raw->logs.size(), 1);
    EXPECT_EQ(raw->logs[0].message, "overridden");
    EXPECT_EQ(raw->logs[0].tags.find("component"), "override");
}

// ── Error-path tests ──────────────────────────────────────────────────────────

TEST_F(TaggedTest, EmptyTaggedInstance) {
    class EmptyTagged : public Tagged {
    public:
        void do_work() {
            LUMEN_MEMBER_SCOPE;
            LOG_INFO("from empty tagged");
        }
    };

    auto sink = std::make_unique<CaptureSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), always());

    EmptyTagged obj;
    obj.do_work();

    wait_flush();

    ASSERT_GE(raw->logs.size(), 1);
    EXPECT_EQ(raw->logs[0].message, "from empty tagged");
}

TEST_F(TaggedTest, MultipleTaggedInstances) {
    MyComponent comp1;
    MyComponent comp2;

    auto sink = std::make_unique<CaptureSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), always());

    comp1.do_work();
    comp2.do_work();

    wait_flush();

    ASSERT_GE(raw->logs.size(), 2);
}

TEST_F(TaggedTest, TaggedLifetimeScope) {
    auto sink = std::make_unique<CaptureSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), always());

    {
        MyComponent comp;
        comp.do_work();
    }

    LOG_INFO("after component destroyed");

    wait_flush();

    ASSERT_GE(raw->logs.size(), 2);
    EXPECT_TRUE(raw->logs[1].tags.find("component").empty());
}

TEST_F(TaggedTest, TagsCapacityLimit) {
    class FullTagged : public Tagged {
    public:
        FullTagged() {
            for (int i = 0; i < 16; ++i) {
                lumen_tag("tag_" + std::to_string(i), "val_" + std::to_string(i));
            }
        }
        void do_work() {
            LUMEN_MEMBER_SCOPE;
            LOG_INFO("full tags");
        }
    };

    auto sink = std::make_unique<CaptureSink>();
    auto* raw = sink.get();
    register_sink(std::move(sink), always());

    FullTagged obj;
    obj.do_work();

    wait_flush();

    ASSERT_GE(raw->logs.size(), 1);
    EXPECT_GE(raw->logs[0].tags.count(), 8);
}

}  // namespace
}  // namespace lumen
