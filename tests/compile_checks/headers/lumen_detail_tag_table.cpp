// lumen/detail/tag_table.hpp alone declares what the context layers use.
#include "lumen/detail/tag_table.hpp"

#include <type_traits>

namespace lumen::detail {

static_assert(std::is_constructible_v<TagTable, std::size_t>);
static_assert(
    std::is_same_v<decltype(TagTable(1).set("k", "v")), std::expected<void, std::error_code>>);
static_assert(std::is_same_v<decltype(TagTable(1).remove("k")), bool>);
static_assert(std::is_same_v<decltype(TagTable(1).clear()), void>);
static_assert(std::is_same_v<decltype(TagTable(1).tags()), std::span<const OwnedTag>>);

}  // namespace lumen::detail
