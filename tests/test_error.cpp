#include <system_error>

#include <gtest/gtest.h>

#include "lumen/detail/error.hpp"

namespace lumen {
namespace {

TEST(LumenErrorTest, ErrorCodeConstruction) {
    std::error_code ec = make_error_code(LumenError::None);
    EXPECT_EQ(ec.value(), 0);
    EXPECT_EQ(ec.category(), lumen_error_category());
}

TEST(LumenErrorTest, ErrorCategoryName) {
    EXPECT_STREQ(lumen_error_category().name(), "lumen");
}

TEST(LumenErrorTest, ErrorMessages) {
    EXPECT_EQ(lumen_error_category().message(0), "no error");
    EXPECT_EQ(lumen_error_category().message(static_cast<int>(LumenError::BufferFull)),
              "ring buffer is full");
    EXPECT_EQ(lumen_error_category().message(static_cast<int>(LumenError::InvalidSinkId)),
              "sink id not found");
}

TEST(LumenErrorTest, IsErrorCodeEnum) {
    EXPECT_TRUE((std::is_error_code_enum_v<LumenError>));
}

TEST(LumenErrorTest, CategoryEquality) {
    std::error_code ec1 = make_error_code(LumenError::None);
    std::error_code ec2 = make_error_code(LumenError::BufferFull);
    EXPECT_EQ(ec1.category(), ec2.category());
}

TEST(LumenErrorTest, ErrorCodeDistinctValues) {
    EXPECT_NE(static_cast<int>(LumenError::None), static_cast<int>(LumenError::BufferFull));
    EXPECT_NE(static_cast<int>(LumenError::BufferFull), static_cast<int>(LumenError::InvalidSinkId));
}

TEST(LumenErrorTest, UnknownErrorMessage) {
    std::string msg = lumen_error_category().message(999);
    EXPECT_NE(msg.find("unknown"), std::string::npos);
}

}  // namespace
}  // namespace lumen
