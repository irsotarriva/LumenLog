// lumen/tag_set.hpp alone declares what it documents.
#include "lumen/tag_set.hpp"

#include <type_traits>

namespace lumen {

static_assert([] {
    TagSet<2> tags;
    const bool added = tags.add("a", "1") && tags.set("a", "2") && !tags.full();
    return added && tags.contains("a") && tags.find("a") == "2" && tags.count() == 1 &&
           tags.begin() + 1 == tags.end();
}());
static_assert(std::is_same_v<decltype(TagSet<2>::Entry{}.key), std::string_view>);
static_assert(std::is_same_v<decltype(OwnedTag{}.key), std::string>);
static_assert(std::is_same_v<decltype(OwnedTag{}.value), std::string>);

}  // namespace lumen
