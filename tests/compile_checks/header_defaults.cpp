// CMake passes the level threshold and the buffer capacities to consumers only
// when they differ from the fallbacks written in the headers (see
// cmake/build_settings.cmake). That is only correct while the two agree: this
// file receives CMake's defaults as LUMEN_CHECK_* and compares them with what
// the headers produce when nothing is defined.
#undef LUMEN_MIN_LEVEL_VALUE
#undef LUMEN_LOG_CAPACITY
#undef LUMEN_METRIC_CAPACITY
#undef LUMEN_PROGRESS_CAPACITY

#include <cstdint>

#include "lumen/limits.hpp"
#include "lumen/macros.hpp"

static_assert(static_cast<int32_t>(lumen::compiled_min_level) == LUMEN_CHECK_MIN_LEVEL_VALUE,
              "LUMEN_MIN_LEVEL_VALUE fallback in macros.hpp differs from the CMake default");
static_assert(lumen::log_queue_capacity == LUMEN_CHECK_LOG_CAPACITY,
              "LUMEN_LOG_CAPACITY fallback in limits.hpp differs from the CMake default");
static_assert(lumen::metric_queue_capacity == LUMEN_CHECK_METRIC_CAPACITY,
              "LUMEN_METRIC_CAPACITY fallback in limits.hpp differs from the CMake default");
static_assert(lumen::progress_queue_capacity == LUMEN_CHECK_PROGRESS_CAPACITY,
              "LUMEN_PROGRESS_CAPACITY fallback in limits.hpp differs from the CMake default");

int main() {
    return 0;
}
