// The "lumen" error category.
#include <gtest/gtest.h>

#include <set>
#include <string>
#include <system_error>

#include "lumen/error.hpp"

namespace lumen {
namespace {

TEST(LumenErrorTest, CategoryIsNamedLumen) {
    EXPECT_STREQ(lumen_error_category().name(), "lumen");
}

TEST(LumenErrorTest, MakeErrorCodeUsesTheLumenCategoryAndTheEnumeratorValue) {
    const std::error_code ec = make_error_code(LumenError::TagTableFull);
    EXPECT_EQ(ec.category(), lumen_error_category());
    EXPECT_EQ(ec.value(), static_cast<int>(LumenError::TagTableFull));
}

TEST(LumenErrorTest, EnumeratorComparesEqualToItsOwnCodeOnly) {
    const std::error_code ec = LumenError::SinkBusy;
    EXPECT_EQ(ec, LumenError::SinkBusy);
    EXPECT_NE(ec, LumenError::InvalidSinkId);
    // The same value in another category is a different error.
    EXPECT_NE(ec, std::error_code(static_cast<int>(LumenError::SinkBusy), std::generic_category()));
}

TEST(LumenErrorTest, NoneIsTheOnlyCodeThatTestsFalse) {
    EXPECT_FALSE(make_error_code(LumenError::None));
    EXPECT_TRUE(make_error_code(LumenError::NullSink));
}

TEST(LumenErrorTest, EveryErrorHasItsOwnMessage) {
    const std::set<std::string> messages = {
        lumen_error_category().message(static_cast<int>(LumenError::None)),
        lumen_error_category().message(static_cast<int>(LumenError::InvalidSinkId)),
        lumen_error_category().message(static_cast<int>(LumenError::NullSink)),
        lumen_error_category().message(static_cast<int>(LumenError::SinkBusy)),
        lumen_error_category().message(static_cast<int>(LumenError::TagTableFull)),
        lumen_error_category().message(static_cast<int>(LumenError::InvalidPredicate)),
        lumen_error_category().message(static_cast<int>(LumenError::InvalidSinkConfig)),
        lumen_error_category().message(static_cast<int>(LumenError::FileOpenFailed)),
        lumen_error_category().message(static_cast<int>(LumenError::IoError)),
        lumen_error_category().message(static_cast<int>(LumenError::NotATerminal)),
        lumen_error_category().message(static_cast<int>(LumenError::CoreShutDown)),
        lumen_error_category().message(static_cast<int>(LumenError::FlushTimedOut)),
    };
    EXPECT_EQ(messages.size(), 12U);
    EXPECT_FALSE(messages.contains("unknown lumen error"));
}

TEST(LumenErrorTest, UnknownValueHasTheUnknownMessage) {
    EXPECT_EQ(lumen_error_category().message(999), "unknown lumen error");
}

}  // namespace
}  // namespace lumen
