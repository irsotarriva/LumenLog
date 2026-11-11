// lumen/detail/scope_stack.hpp alone declares the guards the macros use.
#include "lumen/detail/scope_stack.hpp"

#include <type_traits>
#include <utility>

namespace lumen::detail {

static_assert(std::is_constructible_v<ScopeGuard, std::string_view, std::string_view>);
static_assert(!std::is_copy_constructible_v<ScopeGuard> &&
              !std::is_move_constructible_v<ScopeGuard>);
static_assert(std::is_constructible_v<LoopScope<int32_t>, Core&, std::string_view, int32_t>);
static_assert(std::is_same_v<decltype(std::declval<LoopScope<int32_t>&>().advance()), bool>);
static_assert(std::is_same_v<decltype(std::declval<const LoopScope<int64_t>&>().index()), int64_t>);

}  // namespace lumen::detail
