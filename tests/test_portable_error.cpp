// portable_error (src/portable_error.hpp): the thread-start error a sink's
// create() returns compares equal to std::errc on every standard library.
#include <gtest/gtest.h>

#include <cerrno>
#include <system_error>

#include "lumen/error.hpp"
#include "portable_error.hpp"

namespace lumen::detail {
namespace {

TEST(PortableErrorTest, SystemCategoryErrnoBecomesTheGenericCodeWithTheSameValue) {
    const std::error_code from_libcxx(EAGAIN, std::system_category());

    const std::error_code portable = portable_error(from_libcxx);

    EXPECT_EQ(portable.category(), std::generic_category());
    EXPECT_EQ(portable.value(), EAGAIN);
    EXPECT_EQ(portable, std::make_error_code(std::errc::resource_unavailable_try_again));
}

TEST(PortableErrorTest, GenericAndLumenCodesAreReturnedUnchanged) {
    const std::error_code generic = std::make_error_code(std::errc::not_enough_memory);
    const std::error_code lumen = make_error_code(LumenError::SinkBusy);

    EXPECT_EQ(portable_error(generic), generic);
    EXPECT_EQ(portable_error(lumen), lumen);
}

}  // namespace
}  // namespace lumen::detail
