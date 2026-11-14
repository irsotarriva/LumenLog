// lumen/tagged.hpp alone declares what it documents.
#include "lumen/tagged.hpp"

#include <type_traits>
#include <utility>

namespace lumen {

class TaggedCheck : public Tagged {};

static_assert(std::is_same_v<decltype(std::declval<TaggedCheck&>().lumen_tag("k", "v")),
                             std::expected<void, std::error_code>>);
static_assert(std::is_same_v<decltype(std::declval<TaggedCheck&>().lumen_untag("k")), bool>);
static_assert(std::is_same_v<decltype(std::declval<const TaggedCheck&>().lumen_tags()),
                             std::span<const OwnedTag>>);
static_assert(
    std::is_same_v<decltype(std::declval<const TaggedCheck&>().lumen_scope()), Tagged::Scope>);
static_assert(std::is_constructible_v<Tagged::Scope, const Tagged&>);
static_assert(!std::is_copy_constructible_v<Tagged::Scope>);
static_assert(sizeof(Tagged) == sizeof(void*));

}  // namespace lumen
