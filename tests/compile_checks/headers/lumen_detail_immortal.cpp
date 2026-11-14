// lumen/detail/immortal.hpp alone declares what it documents.
#include "lumen/detail/immortal.hpp"

#include <string>
#include <type_traits>
#include <utility>

namespace lumen::detail {

static_assert(std::is_same_v<decltype(std::declval<Immortal<std::string>&>().get()), std::string&>);
static_assert(std::is_constructible_v<Immortal<std::string>, const char*>);
static_assert(!std::is_copy_constructible_v<Immortal<std::string>> &&
              !std::is_move_constructible_v<Immortal<std::string>>);

}  // namespace lumen::detail
