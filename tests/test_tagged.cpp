// lumen::Tagged: instance tags, their limit, copy and move, and member scopes.
#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

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

using Pairs = std::vector<std::pair<std::string, std::string>>;

class Detector : public Tagged {
public:
    Detector() = default;
    explicit Detector(const std::string& name) { EXPECT_TRUE(lumen_tag("detector", name)); }

    void emit(Core& core, std::string_view message) const {
        LUMEN_MEMBER_SCOPE;
        log_to(core, message);
    }
};

[[nodiscard]] Pairs pairs_of(const Tagged& object) {
    Pairs out;
    for (const auto& tag : object.lumen_tags()) {
        out.emplace_back(tag.key, tag.value);
    }
    return out;
}

static_assert(sizeof(Tagged) == sizeof(void*), "Tagged is one pointer");
static_assert(!std::is_polymorphic_v<Tagged>, "Tagged has no virtual functions");
static_assert(!std::is_constructible_v<Tagged>, "Tagged is only a base class");

class TaggedTest : public LocalCoreTest {
protected:
    [[nodiscard]] std::vector<LogRecord> _logs() {
        _core.flush();
        return _captured->logs();
    }
};

TEST_F(TaggedTest, RecordsInsideAMemberScopeCarryTheObjectsTags) {
    const Detector d("tracker");
    d.emit(_core, "inside");
    log_to(_core, "outside");
    const auto records = _logs();
    ASSERT_EQ(records.size(), 2U);
    EXPECT_EQ(tag_of(records[0].tags, "detector"), "tracker");
    EXPECT_EQ(tag_of(records[1].tags, "detector"), std::nullopt);
}

TEST_F(TaggedTest, TwoInstancesWithDifferentTagsTagTheirOwnRecords) {
    const Detector a("tracker");
    const Detector b("calorimeter");
    a.emit(_core, "a");
    b.emit(_core, "b");
    const auto records = _logs();
    ASSERT_EQ(records.size(), 2U);
    EXPECT_EQ(tag_of(records[0].tags, "detector"), "tracker");
    EXPECT_EQ(tag_of(records[1].tags, "detector"), "calorimeter");
}

TEST_F(TaggedTest, UntaggedObjectAddsNoTags) {
    const Detector d;
    d.emit(_core, "plain");
    const auto records = _logs();
    ASSERT_EQ(records.size(), 1U);
    EXPECT_EQ(records[0].tags.count(), 0U);
}

TEST_F(TaggedTest, TaggingAKeyAgainReplacesItsValue) {
    Detector d("old");
    ASSERT_TRUE(d.lumen_tag("detector", "new"));
    EXPECT_EQ(pairs_of(d), (Pairs{{"detector", "new"}}));
}

TEST_F(TaggedTest, TagPastTheInstanceLimitFailsWithTagTableFullAndChangesNothing) {
    Detector d;
    for (std::size_t i = 0; i < max_instance_tags; ++i) {
        ASSERT_TRUE(d.lumen_tag("k" + std::to_string(i), "v"));
    }
    const auto refused = d.lumen_tag("one_too_many", "v");
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error(), LumenError::TagTableFull);
    EXPECT_EQ(d.lumen_tags().size(), max_instance_tags);
    EXPECT_TRUE(d.lumen_tag("k0", "replacing still works"));
}

TEST_F(TaggedTest, UntagRemovesAKeyAndReportsWhetherItWasThere) {
    Detector d("x");
    EXPECT_TRUE(d.lumen_untag("detector"));
    EXPECT_FALSE(d.lumen_untag("detector"));
    EXPECT_TRUE(d.lumen_tags().empty());
}

TEST_F(TaggedTest, ExplicitTagWinsOverTheInstanceTag) {
    const Detector d("instance");
    {
        const auto scope = d.lumen_scope();
        log_to(_core, "explicit").tag("detector", "explicit");
    }
    const auto records = _logs();
    ASSERT_EQ(records.size(), 1U);
    EXPECT_EQ(tag_of(records[0].tags, "detector"), "explicit");
}

TEST_F(TaggedTest, InnerMemberScopeWinsAndOuterObjectsOtherTagsRemain) {
    Detector outer("outer");
    ASSERT_TRUE(outer.lumen_tag("run", "7"));
    const Detector inner("inner");
    {
        const auto outer_scope = outer.lumen_scope();
        inner.emit(_core, "nested");
    }
    const auto records = _logs();
    ASSERT_EQ(records.size(), 1U);
    EXPECT_EQ(tags_of(records[0].tags), (Pairs{{"detector", "inner"}, {"run", "7"}}));
}

TEST_F(TaggedTest, TagSetInsideAMemberScopeAppliesToLaterRecords) {
    Detector d;
    {
        const auto scope = d.lumen_scope();
        log_to(_core, "before");
        ASSERT_TRUE(d.lumen_tag("stage", "2"));
        log_to(_core, "after");
    }
    const auto records = _logs();
    ASSERT_EQ(records.size(), 2U);
    EXPECT_EQ(tag_of(records[0].tags, "stage"), std::nullopt);
    EXPECT_EQ(tag_of(records[1].tags, "stage"), "2");
}

TEST_F(TaggedTest, CopyHasItsOwnTagsIndependentOfTheOriginal) {
    Detector original("a");
    Detector copy(original);
    ASSERT_TRUE(copy.lumen_tag("detector", "b"));
    EXPECT_EQ(pairs_of(original), (Pairs{{"detector", "a"}}));
    EXPECT_EQ(pairs_of(copy), (Pairs{{"detector", "b"}}));

    Detector assigned;
    assigned = original;
    ASSERT_TRUE(original.lumen_tag("detector", "c"));
    EXPECT_EQ(pairs_of(assigned), (Pairs{{"detector", "a"}}));
}

TEST_F(TaggedTest, MoveTransfersTheTagsAndLeavesTheSourceUntagged) {
    Detector source("moved");
    Detector target(std::move(source));
    EXPECT_EQ(pairs_of(target), (Pairs{{"detector", "moved"}}));
    // NOLINTNEXTLINE(bugprone-use-after-move): the moved-from state is specified
    EXPECT_TRUE(source.lumen_tags().empty());

    Detector assigned("previous");
    assigned = std::move(target);
    EXPECT_EQ(pairs_of(assigned), (Pairs{{"detector", "moved"}}));
}

TEST_F(TaggedTest, CopyOfAnUntaggedObjectIsUntagged) {
    const Detector empty;
    const Detector copy(empty);  // NOLINT(performance-unnecessary-copy-initialization)
    EXPECT_TRUE(copy.lumen_tags().empty());
}

}  // namespace
}  // namespace lumen
