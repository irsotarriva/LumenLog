// LogLevel, TagSet and the thread/clock stamps: the value types records are
// made of.
#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "lumen/level.hpp"
#include "lumen/tag_set.hpp"
#include "lumen/thread_clock.hpp"

namespace lumen {
namespace {

TEST(LogLevelTest, LevelsAreOrderedTraceToFatalWithValuesZeroToFive) {
    EXPECT_EQ(static_cast<int>(LogLevel::Trace), 0);
    EXPECT_EQ(static_cast<int>(LogLevel::Debug), 1);
    EXPECT_EQ(static_cast<int>(LogLevel::Info), 2);
    EXPECT_EQ(static_cast<int>(LogLevel::Warn), 3);
    EXPECT_EQ(static_cast<int>(LogLevel::Error), 4);
    EXPECT_EQ(static_cast<int>(LogLevel::Fatal), 5);
}

TEST(TagSetTest, DefaultConstructedSetIsEmpty) {
    const TagSet<4> tags;
    EXPECT_EQ(tags.count(), 0U);
    EXPECT_EQ(tags.begin(), tags.end());
    EXPECT_FALSE(tags.contains("k"));
}

TEST(TagSetTest, AddAppendsPairsInOrder) {
    TagSet<4> tags;
    EXPECT_TRUE(tags.add("a", "1"));
    EXPECT_TRUE(tags.add("b", "2"));
    ASSERT_EQ(tags.count(), 2U);
    EXPECT_EQ(tags.begin()[0].key, "a");
    EXPECT_EQ(tags.begin()[0].value, "1");
    EXPECT_EQ(tags.begin()[1].key, "b");
    EXPECT_EQ(tags.begin()[1].value, "2");
}

TEST(TagSetTest, AddKeepsADuplicateKeyAndFindReturnsTheFirstValue) {
    TagSet<4> tags;
    EXPECT_TRUE(tags.add("k", "first"));
    EXPECT_TRUE(tags.add("k", "second"));
    EXPECT_EQ(tags.count(), 2U);
    EXPECT_EQ(tags.find("k"), "first");
}

TEST(TagSetTest, SetReplacesTheValueOfAnExistingKey) {
    TagSet<4> tags;
    EXPECT_TRUE(tags.set("k", "old"));
    EXPECT_TRUE(tags.set("k", "new"));
    EXPECT_EQ(tags.count(), 1U);
    EXPECT_EQ(tags.find("k"), "new");
}

TEST(TagSetTest, AddOnAFullSetReturnsFalseAndChangesNothing) {
    TagSet<2> tags;
    EXPECT_TRUE(tags.add("a", "1"));
    EXPECT_TRUE(tags.add("b", "2"));
    EXPECT_TRUE(tags.full());
    EXPECT_FALSE(tags.add("c", "3"));
    EXPECT_EQ(tags.count(), 2U);
    EXPECT_FALSE(tags.contains("c"));
}

TEST(TagSetTest, SetOnAFullSetReplacesAnExistingKeyButRefusesANewOne) {
    TagSet<1> tags;
    EXPECT_TRUE(tags.set("a", "1"));
    EXPECT_TRUE(tags.set("a", "2"));
    EXPECT_EQ(tags.find("a"), "2");
    EXPECT_FALSE(tags.set("b", "3"));
    EXPECT_FALSE(tags.contains("b"));
}

TEST(TagSetTest, FindDistinguishesAnEmptyValueFromAMissingKeyThroughContains) {
    TagSet<4> tags;
    EXPECT_TRUE(tags.add("empty", ""));
    EXPECT_TRUE(tags.contains("empty"));
    EXPECT_EQ(tags.find("empty"), "");
    EXPECT_FALSE(tags.contains("missing"));
    EXPECT_EQ(tags.find("missing"), "");
}

TEST(TagSetTest, EmptyKeyIsAnOrdinaryKey) {
    TagSet<4> tags;
    EXPECT_TRUE(tags.add("", "value"));
    EXPECT_TRUE(tags.contains(""));
    EXPECT_EQ(tags.find(""), "value");
}

TEST(TagSetTest, RemapRepointsEveryKeyAndValue) {
    TagSet<4> tags;
    EXPECT_TRUE(tags.add("a", "1"));
    EXPECT_TRUE(tags.add("b", "2"));
    std::vector<std::string> storage;
    storage.reserve(4);
    tags.remap([&](std::string_view s) -> std::string_view {
        storage.emplace_back(std::string(s) + "!");
        return storage.back();
    });
    EXPECT_EQ(tags.find("a!"), "1!");
    EXPECT_EQ(tags.find("b!"), "2!");
    EXPECT_FALSE(tags.contains("a"));
}

TEST(ThreadIdTest, IdIsNonZeroAndStableWithinAThread) {
    const uint32_t first = this_thread_id();
    EXPECT_NE(first, 0U);
    EXPECT_EQ(this_thread_id(), first);
}

TEST(ThreadIdTest, SixtyFourThreadsGetSixtyFourDistinctIds) {
    constexpr std::size_t thread_count = 64;
    std::vector<uint32_t> ids(thread_count);
    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < thread_count; ++i) {
        threads.emplace_back([&ids, i] { ids[i] = this_thread_id(); });
    }
    for (std::thread& t : threads)
        t.join();
    ids.push_back(this_thread_id());
    const std::set<uint32_t> distinct(ids.begin(), ids.end());
    EXPECT_EQ(distinct.size(), thread_count + 1);
    EXPECT_FALSE(distinct.contains(0U));
}

}  // namespace
}  // namespace lumen
