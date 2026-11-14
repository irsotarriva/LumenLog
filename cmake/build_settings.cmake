# Compile-time settings that change what the public headers compile to.
#
# They reach the code through one generated header, lumen/config.hpp
# (template: cmake/config.hpp.in), which is installed with the other headers.
# No definition is put on the command line of the library or of a consumer,
# so a consumer that does not use CMake sees the same values as one that
# links lumen::lumen.

# Lowest level compiled in. The header receives the level's numeric value, not
# its name: a name would be macro-expanded in every consumer that defines DEBUG
# or ERROR itself.
set(lumen_level_names TRACE DEBUG INFO WARN ERROR FATAL)
set(LUMEN_MIN_LEVEL "DEBUG" CACHE STRING
    "Elides records below this level at compile time (TRACE, DEBUG, INFO, WARN, ERROR, FATAL)")
set_property(CACHE LUMEN_MIN_LEVEL PROPERTY STRINGS ${lumen_level_names})
string(TOUPPER "${LUMEN_MIN_LEVEL}" lumen_min_level_upper)
list(FIND lumen_level_names "${lumen_min_level_upper}" lumen_min_level_value)
if(lumen_min_level_value EQUAL -1)
  string(JOIN ", " lumen_level_list ${lumen_level_names})
  message(FATAL_ERROR "LUMEN_MIN_LEVEL='${LUMEN_MIN_LEVEL}' is not a level name."
                      " Use one of: ${lumen_level_list}")
endif()

# Ring buffer slot counts, read by lumen/limits.hpp.
set(LUMEN_LOG_CAPACITY "65536" CACHE STRING "LogBuffer slot count (power of two)")
set(LUMEN_METRIC_CAPACITY "16384" CACHE STRING "MetricBuffer slot count (power of two)")
set(LUMEN_PROGRESS_CAPACITY "4096" CACHE STRING "ProgressBuffer slot count (power of two)")
foreach(kind IN ITEMS LOG METRIC PROGRESS)
  set(capacity "${LUMEN_${kind}_CAPACITY}")
  if(NOT capacity MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "LUMEN_${kind}_CAPACITY='${capacity}' is not a positive integer")
  endif()
  # A power of two has a single bit set: n & (n - 1) is 0.
  math(EXPR lumen_low_bits "${capacity} & (${capacity} - 1)")
  if(NOT lumen_low_bits EQUAL 0)
    message(FATAL_ERROR "LUMEN_${kind}_CAPACITY=${capacity} is not a power of two")
  endif()
endforeach()

set(lumen_config_header "${PROJECT_BINARY_DIR}/include/lumen/config.hpp")
configure_file("${CMAKE_CURRENT_LIST_DIR}/config.hpp.in" "${lumen_config_header}" @ONLY)
target_include_directories(lumen PUBLIC $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>)
