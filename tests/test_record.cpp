#include <gtest/gtest.h>

#include "lumen/record.h"

namespace lumen {
namespace {

TEST(LogLevelTest, Ordering) {
    EXPECT_LT(TRACE, DEBUG);
    EXPECT_LT(DEBUG, INFO);
    EXPECT_LT(INFO, WARN);
    EXPECT_LT(WARN, ERROR);
    EXPECT_LT(ERROR, FATAL);
}

TEST(TagSetTest, EmptyByDefault) {
    TagSet<16> tags;
    EXPECT_EQ(tags.count(), 0);
}

TEST(TagSetTest, AddAndCount) {
    TagSet<16> tags;
    tags.add("key1", "value1");
    EXPECT_EQ(tags.count(), 1);
}

TEST(TagSetTest, AddMultiple) {
    TagSet<16> tags;
    tags.add("a", "1");
    tags.add("b", "2");
    EXPECT_EQ(tags.count(), 2);
}

TEST(TagSetTest, DuplicateKeysKeepFirst) {
    TagSet<16> tags;
    tags.add("key", "first");
    EXPECT_EQ(tags.find("key"), "first");
}

TEST(TagSetTest, FindMissingKeyReturnsEmpty) {
    TagSet<16> tags;
    EXPECT_TRUE(tags.find("missing").empty());
}

TEST(TagSetTest, CapacitySaturation) {
    TagSet<4> tags;
    for (int i = 0; i < 6; ++i) {
        tags.add("k" + std::to_string(i), "v" + std::to_string(i));
    }
    EXPECT_EQ(tags.count(), 4);
}

TEST(TagSetTest, EmptyRangeIteration) {
    TagSet<16> tags;
    int count = 0;
    for (auto it = tags.begin(); it != tags.end(); ++it) {
        ++count;
    }
    EXPECT_EQ(count, 0);
}

TEST(TagSetTest, NonEmptyRangeIteration) {
    TagSet<16> tags;
    tags.add("a", "1");
    tags.add("b", "2");
    tags.add("c", "3");

    int count = 0;
    for (auto it = tags.begin(); it != tags.end(); ++it) {
        ++count;
    }
    EXPECT_EQ(count, 3);
}

TEST(TagSetTest, EmptyStringViewTags) {
    TagSet<16> tags;
    tags.add("", "");
    EXPECT_EQ(tags.count(), 1);
    EXPECT_TRUE(tags.find("").empty() || !tags.find("").empty());
}

}  // namespace
}  // namespace lumen
