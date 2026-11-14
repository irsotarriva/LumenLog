// lumen/thread_clock.hpp alone declares what it documents.
#include "lumen/thread_clock.hpp"

#include <type_traits>

namespace lumen {

static_assert(std::is_same_v<decltype(now_ns()), uint64_t>);
static_assert(std::is_same_v<decltype(this_thread_id()), uint32_t>);

}  // namespace lumen
