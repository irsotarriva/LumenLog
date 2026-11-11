// lumen/progress.hpp alone declares what it documents.
#include "lumen/progress.hpp"

#include <type_traits>

namespace lumen {

static_assert(std::is_same_v<decltype(progress("files", 10)), ProgressHandle>);
static_assert(std::is_constructible_v<ProgressHandle, Core&, std::string_view, uint64_t>);
static_assert(std::is_nothrow_move_constructible_v<ProgressHandle>);
static_assert(std::is_nothrow_move_assignable_v<ProgressHandle>);
static_assert(!std::is_copy_constructible_v<ProgressHandle>);
static_assert(std::is_same_v<decltype(progress("f", 1).update(1)), void>);
static_assert(std::is_same_v<decltype(progress("f", 1).tick()), void>);
static_assert(std::is_same_v<decltype(progress("f", 1).finish()), void>);
static_assert(std::is_same_v<decltype(progress("f", 1).current()), uint64_t>);
static_assert(std::is_same_v<decltype(progress("f", 1).total()), uint64_t>);
static_assert(std::is_same_v<decltype(progress("f", 1).finished()), bool>);

}  // namespace lumen
