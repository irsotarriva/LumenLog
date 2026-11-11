// lumen/macros.hpp alone declares what it documents. The macros themselves
// are expanded by macros_alone.cpp, which also runs them.
#include "lumen/macros.hpp"

#include <type_traits>

namespace lumen {

static_assert(std::is_same_v<decltype(compiled_min_level), const LogLevel>);
static_assert(level_compiled_in(LogLevel::Fatal));
static_assert(LUMEN_MIN_LEVEL_VALUE >= 0 && LUMEN_MIN_LEVEL_VALUE <= 5);

}  // namespace lumen
