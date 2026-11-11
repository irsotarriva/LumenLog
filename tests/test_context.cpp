// Ambient context: the process and thread layers, their limits and removal,
// and how all layers merge into a record.
#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "lumen/context.hpp"
#include "lumen/error.hpp"
#include "lumen/limits.hpp"
#include "lumen/macros.hpp"
#include "lumen/tagged.hpp"
#include "test_support.hpp"

namespace lumen {
namespace {

using test::LocalCoreTest;
using test::log_to;
using test::tag_of;
using test::tags_of;

class ContextTest : public LocalCoreTest {
protected:
    // Submits one record and returns its tags as delivered (the last record captured).
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> _tags_of_one_record() {
        log_to(_core, "probe");
        _core.flush();
        const auto logs = _captured->logs();
        EXPECT_FALSE(logs.empty());
        if (logs.empty()) {
            return {};
        }
        return tags_of(logs.back().tags);
    }
    [[nodiscard]] std::optional<std::string> _tag_on_one_record(std::string_view key) {
        log_to(_core, "probe");
        _core.flush();
        const auto logs = _captured->logs();
        EXPECT_FALSE(logs.empty());
        if (logs.empty()) {
            return std::nullopt;
        }
        return tag_of(logs.back().tags, key);
    }
};

class Widget : public Tagged {};

TEST_F(ContextTest, ProcessTagReachesRecordsFromEveryThread) {
    ASSERT_TRUE(set_process_tag("app", "demo"));
    std::thread other([this] { log_to(_core, "other thread"); });
    other.join();
    log_to(_core, "this thread");
    _core.flush();
    const auto logs = _captured->logs();
    ASSERT_EQ(logs.size(), 2U);
    EXPECT_EQ(tag_of(logs[0].tags, "app"), "demo");
    EXPECT_EQ(tag_of(logs[1].tags, "app"), "demo");
}

TEST_F(ContextTest, ThreadTagReachesOnlyRecordsOfTheThreadThatSetIt) {
    ASSERT_TRUE(set_thread_tag("worker", "main"));
    std::thread other([this] { log_to(_core, "other thread"); });
    other.join();
    log_to(_core, "this thread");
    _core.flush();
    const auto logs = _captured->logs();
    ASSERT_EQ(logs.size(), 2U);
    EXPECT_EQ(tag_of(logs[0].tags, "worker"), std::nullopt);
    EXPECT_EQ(tag_of(logs[1].tags, "worker"), "main");
}

TEST_F(ContextTest, SettingAProcessTagAgainReplacesItsValue) {
    ASSERT_TRUE(set_process_tag("k", "old"));
    ASSERT_TRUE(set_process_tag("k", "new"));
    EXPECT_EQ(_tags_of_one_record(),
              (std::vector<std::pair<std::string, std::string>>{{"k", "new"}}));
}

TEST_F(ContextTest, SettingAThreadTagAgainReplacesItsValue) {
    ASSERT_TRUE(set_thread_tag("k", "old"));
    ASSERT_TRUE(set_thread_tag("k", "new"));
    EXPECT_EQ(_tags_of_one_record(),
              (std::vector<std::pair<std::string, std::string>>{{"k", "new"}}));
}

TEST_F(ContextTest, ProcessTableRefusesTheTagPastItsLimitAndKeepsTheFirstOnes) {
    for (std::size_t i = 0; i < max_process_tags; ++i) {
        ASSERT_TRUE(set_process_tag("p" + std::to_string(i), "v"));
    }
    const auto refused = set_process_tag("one_too_many", "v");
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error(), LumenError::TagTableFull);
    // Replacing an existing key still works on a full table.
    EXPECT_TRUE(set_process_tag("p0", "replaced"));
    const auto tags = _tags_of_one_record();
    ASSERT_EQ(tags.size(), max_process_tags);
    EXPECT_EQ(tags[0], (std::pair<std::string, std::string>{"p0", "replaced"}));
    EXPECT_EQ(tags.back().first, "p" + std::to_string(max_process_tags - 1));
}

TEST_F(ContextTest, ThreadTableRefusesTheTagPastItsLimit) {
    for (std::size_t i = 0; i < max_thread_tags; ++i) {
        ASSERT_TRUE(set_thread_tag("t" + std::to_string(i), "v"));
    }
    const auto refused = set_thread_tag("one_too_many", "v");
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error(), LumenError::TagTableFull);
    EXPECT_EQ(_tags_of_one_record().size(), max_thread_tags);
}

TEST_F(ContextTest, RemovedProcessTagIsNoLongerOnRecords) {
    ASSERT_TRUE(set_process_tag("gone", "1"));
    ASSERT_TRUE(set_process_tag("kept", "2"));
    // A record first, so this thread holds a current copy of the process tags
    // that the removal has to invalidate.
    EXPECT_EQ(_tag_on_one_record("gone"), "1");
    EXPECT_TRUE(remove_process_tag("gone"));
    // Checked before anything else changes the process tags, so that only
    // the removal can have made the record current.
    EXPECT_EQ(_tags_of_one_record(),
              (std::vector<std::pair<std::string, std::string>>{{"kept", "2"}}));
}

TEST_F(ContextTest, RemovingAMissingProcessTagReturnsFalseAndLeavesTheOthers) {
    ASSERT_TRUE(set_process_tag("kept", "2"));
    EXPECT_FALSE(remove_process_tag("never_set"));
    EXPECT_EQ(_tags_of_one_record(),
              (std::vector<std::pair<std::string, std::string>>{{"kept", "2"}}));
}

TEST_F(ContextTest, RemovedThreadTagIsNoLongerOnRecords) {
    ASSERT_TRUE(set_thread_tag("gone", "1"));
    ASSERT_TRUE(set_thread_tag("kept", "2"));
    EXPECT_TRUE(remove_thread_tag("gone"));
    EXPECT_FALSE(remove_thread_tag("never_set"));
    EXPECT_EQ(_tags_of_one_record(),
              (std::vector<std::pair<std::string, std::string>>{{"kept", "2"}}));
}

TEST_F(ContextTest, ClearingBothLayersLeavesRecordsWithoutAmbientTags) {
    ASSERT_TRUE(set_process_tag("p", "1"));
    ASSERT_TRUE(set_thread_tag("t", "1"));
    clear_process_tags();
    clear_thread_tags();
    EXPECT_TRUE(_tags_of_one_record().empty());
}

TEST_F(ContextTest, ClearedTableAcceptsAFullSetOfTagsAgain) {
    for (std::size_t i = 0; i < max_thread_tags; ++i) {
        ASSERT_TRUE(set_thread_tag("t" + std::to_string(i), "v"));
    }
    clear_thread_tags();
    for (std::size_t i = 0; i < max_thread_tags; ++i) {
        EXPECT_TRUE(set_thread_tag("u" + std::to_string(i), "v"));
    }
}

TEST_F(ContextTest, SameKeyInEveryLayerTakesTheValueOfTheHighestLayer) {
    ASSERT_TRUE(set_process_tag("k", "process"));
    ASSERT_TRUE(set_thread_tag("k", "thread"));
    EXPECT_EQ(_tag_on_one_record("k"), "thread");
    {
        LUMEN_SCOPE("k", "scope");
        EXPECT_EQ(_tag_on_one_record("k"), "scope");
        Widget w;
        ASSERT_TRUE(w.lumen_tag("k", "instance"));
        const auto member_scope = w.lumen_scope();
        EXPECT_EQ(_tag_on_one_record("k"), "instance");
        log_to(_core, "explicit").tag("k", "explicit");
        _core.flush();
        EXPECT_EQ(tag_of(_captured->logs().back().tags, "k"), "explicit");
    }
    EXPECT_EQ(_tag_on_one_record("k"), "thread");
}

TEST_F(ContextTest, FullRecordKeepsHigherLayersAndCountsEveryAmbientTagLeftOut) {
    // 10 explicit + 8 thread + 16 process = 34 candidates for 16 places.
    for (std::size_t i = 0; i < max_process_tags; ++i) {
        ASSERT_TRUE(set_process_tag("p" + std::to_string(i), "v"));
    }
    for (std::size_t i = 0; i < max_thread_tags; ++i) {
        ASSERT_TRUE(set_thread_tag("t" + std::to_string(i), "v"));
    }
    {
        auto builder = log_to(_core, "crowded");
        for (int i = 0; i < 10; ++i) {
            builder.tag("e" + std::to_string(i), i);
        }
    }
    _core.flush();
    const auto logs = _captured->logs();
    ASSERT_EQ(logs.size(), 1U);
    const auto tags = tags_of(logs[0].tags);
    ASSERT_EQ(tags.size(), max_record_tags);
    EXPECT_EQ(tags[0].first, "e0");
    EXPECT_EQ(tags[9].first, "e9");
    EXPECT_EQ(tags[10].first, "t0");
    EXPECT_EQ(tags[15].first, "t5");
    EXPECT_EQ(tag_of(logs[0].tags, "t6"), std::nullopt);
    EXPECT_EQ(tag_of(logs[0].tags, "p0"), std::nullopt);
    EXPECT_EQ(_core.statistics().tags_dropped, 2U + max_process_tags);
    const auto lines = _diagnostic_lines();
    ASSERT_EQ(lines.size(), 1U);
    EXPECT_NE(lines[0].find("tag was dropped"), std::string::npos);
}

TEST_F(ContextTest, AmbientTagShadowedByAnExplicitTagIsNotCountedAsDropped) {
    ASSERT_TRUE(set_thread_tag("k", "thread"));
    log_to(_core, "shadowed").tag("k", "explicit");
    _core.flush();
    EXPECT_EQ(_core.statistics().tags_dropped, 0U);
}

TEST_F(ContextTest, TagBytesArriveUnchangedWhateverTheyContain) {
    const std::vector<std::pair<std::string, std::string>> unusual = {
        {"equals", "a=b=c"},
        {"with space", "two words"},
        {"newline", "line1\nline2"},
        {"quotes", "\"double\" and 'single'"},
        {"utf8", "\xce\xbb = 532 nm, \xe2\x82\xac"},
        {"latin1", "\xe9\xff"},
        {std::string("nul\0inside", 10), std::string("a\0b", 3)},
    };
    {
        auto builder = log_to(_core, "unusual");
        for (const auto& [k, v] : unusual) {
            builder.tag(k, v);
        }
    }
    ASSERT_TRUE(set_process_tag("process =\n\"", "\x01\x1b[0m"));
    _core.flush();
    const auto logs = _captured->logs();
    ASSERT_EQ(logs.size(), 1U);
    // The process tag was set after this record was submitted, so it is not on it.
    EXPECT_EQ(tags_of(logs[0].tags), unusual);

    log_to(_core, "after");
    _core.flush();
    EXPECT_EQ(tag_of(_captured->logs().back().tags, "process =\n\""), "\x01\x1b[0m");
}

}  // namespace
}  // namespace lumen
