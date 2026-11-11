// lumen/config.hpp is generated from the CMake options. This file receives
// the values CMake was configured with as LUMEN_CHECK_* and compares them
// with what the headers produce.
#include <cstdint>

#include "lumen/limits.hpp"
#include "lumen/macros.hpp"

static_assert(static_cast<int32_t>(lumen::compiled_min_level) == LUMEN_CHECK_MIN_LEVEL_VALUE,
              "lumen/config.hpp holds another LUMEN_MIN_LEVEL than CMake was configured with");
static_assert(lumen::log_queue_capacity == LUMEN_CHECK_LOG_CAPACITY,
              "lumen/config.hpp holds another LUMEN_LOG_CAPACITY than CMake was configured with");
static_assert(
    lumen::metric_queue_capacity == LUMEN_CHECK_METRIC_CAPACITY,
    "lumen/config.hpp holds another LUMEN_METRIC_CAPACITY than CMake was configured with");
static_assert(
    lumen::progress_queue_capacity == LUMEN_CHECK_PROGRESS_CAPACITY,
    "lumen/config.hpp holds another LUMEN_PROGRESS_CAPACITY than CMake was configured with");

int main() {
    return 0;
}
