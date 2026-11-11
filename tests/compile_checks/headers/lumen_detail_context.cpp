// lumen/detail/context.hpp alone declares what the guards and Core use.
#include "lumen/detail/context.hpp"

#include <type_traits>
#include <utility>

namespace lumen::detail {

static_assert(std::is_same_v<decltype(push_scope("k", "v")), FrameToken>);
static_assert(std::is_same_v<decltype(pop_scope(FrameToken{})), void>);
static_assert(std::is_same_v<decltype(push_instance(std::declval<const Tagged&>())), FrameToken>);
static_assert(std::is_same_v<decltype(pop_instance(FrameToken{})), void>);
static_assert(std::is_same_v<decltype(instance_tags(std::declval<const Tagged&>())),
                             std::span<const OwnedTag>>);
static_assert(std::is_same_v<decltype(merge_context(std::declval<RecordTags&>(),
                                                    std::declval<ContextPin&>())),
                             uint32_t>);

}  // namespace lumen::detail
