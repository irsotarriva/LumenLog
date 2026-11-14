// lumen/detail/string_scratch.hpp alone declares what the builders use.
#include "lumen/detail/string_scratch.hpp"

#include <type_traits>
#include <utility>

namespace lumen::detail {

using Scratch = StringScratch;
static_assert(std::is_same_v<decltype(std::declval<Scratch&>().store("text")), std::string_view>);
static_assert(
    std::is_same_v<decltype(std::declval<Scratch&>().store_number(1.5)), std::string_view>);
static_assert(
    std::is_same_v<decltype(std::declval<Scratch&>().store_formatted("{}", 1)), std::string_view>);
static_assert(Scratch::inline_capacity > 0);

}  // namespace lumen::detail
