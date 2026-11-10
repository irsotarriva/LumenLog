// The lumen target defines LUMEN_MIN_LEVEL_VALUE for its consumers when
// LUMEN_MIN_LEVEL is not the default; remove it to see what a build without
// the CMake define gets.
#undef LUMEN_MIN_LEVEL_VALUE

#include "lumen/macros.hpp"

static_assert(lumen::compiled_min_level == lumen::LogLevel::Debug,
              "without LUMEN_MIN_LEVEL_VALUE the threshold must default to Debug");
static_assert(!lumen::level_compiled_in(lumen::LogLevel::Trace));
static_assert(lumen::level_compiled_in(lumen::LogLevel::Debug));

int main() {
    return 0;
}
