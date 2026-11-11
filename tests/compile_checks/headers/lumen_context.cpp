// lumen/context.hpp alone declares what it documents.
#include "lumen/context.hpp"

#include <type_traits>

namespace lumen {

using SetResult = std::expected<void, std::error_code>;
static_assert(std::is_same_v<decltype(set_process_tag("k", "v")), SetResult>);
static_assert(std::is_same_v<decltype(remove_process_tag("k")), bool>);
static_assert(std::is_same_v<decltype(clear_process_tags()), void>);
static_assert(std::is_same_v<decltype(set_thread_tag("k", "v")), SetResult>);
static_assert(std::is_same_v<decltype(remove_thread_tag("k")), bool>);
static_assert(std::is_same_v<decltype(clear_thread_tags()), void>);

}  // namespace lumen
