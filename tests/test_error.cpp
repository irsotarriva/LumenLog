#include <system_error>

#include <gtest/gtest.h>

#include "lumen/detail/error.h"

namespace lumen {
namespace {

TEST(LumenErrorTest, ErrorCodeConstruction) {
    std::error_code ec = make_error_code(LumenError::none);
    EXPECT_EQ(ec.value(), 0);
    EXPECT_EQ(ec.category(), lumen_error_category());
}

TEST(LumenErrorTest, ErrorCategoryName) {
    EXPECT_STREQ(lumen_error_category().name(), "lumen");
}

TEST(LumenErrorTest, ErrorMessages) {
    EXPECT_EQ(lumen_error_category().message(0), "no error");
    EXPECT_EQ(lumen_error_category().message(static_cast<int>(LumenError::buffer_full)),
              "ring buffer is full");
    EXPECT_EQ(lumen_error_category().message(static_cast<int>(LumenError::invalid_sink_id)),
              "sink id not found");
}

TEST(LumenErrorTest, IsErrorCodeEnum) {
    EXPECT_TRUE((std::is_error_code_enum_v<LumenError>));
}

TEST(LumenErrorTest, CategoryEquality) {
    std::error_code ec1 = make_error_code(LumenError::none);
    std::error_code ec2 = make_error_code(LumenError::buffer_full);
    EXPECT_EQ(ec1.category(), ec2.category());
}

TEST(LumenErrorTest, ErrorCodeDistinctValues) {
    EXPECT_NE(static_cast<int>(LumenError::none), static_cast<int>(LumenError::buffer_full));
    EXPECT_NE(static_cast<int>(LumenError::buffer_full), static_cast<int>(LumenError::invalid_sink_id));
}

TEST(LumenErrorTest, UnknownErrorMessage) {
    std::string msg = lumen_error_category().message(999);
    EXPECT_NE(msg.find("unknown"), std::string::npos);
}

}  // namespace
}  // namespace lumen
