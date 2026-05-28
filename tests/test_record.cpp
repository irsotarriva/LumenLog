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

}  // namespace
}  // namespace lumen
