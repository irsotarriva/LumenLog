# Compile-time settings that change what the public headers compile to.
#
# Each one is passed as a PUBLIC definition, so that the library and every
# consumer agree, but only when it differs from the default written in the
# header. A default build therefore puts no Lumen macro on a consumer's command
# line, and the installed package works for a consumer that does not use CMake.
# The *_default values below must equal the fallbacks in the headers named.

# Lowest level compiled in. The header receives the level's numeric value, not
# its name: a name would be macro-expanded in every consumer that defines DEBUG
# or ERROR itself. Header default: LUMEN_MIN_LEVEL_VALUE in lumen/macros.hpp.
set(lumen_level_names TRACE DEBUG INFO WARN ERROR FATAL)
set(lumen_min_level_value_default 1)
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
if(NOT lumen_min_level_value EQUAL lumen_min_level_value_default)
  target_compile_definitions(lumen PUBLIC LUMEN_MIN_LEVEL_VALUE=${lumen_min_level_value})
endif()

# Ring buffer slot counts. Consumers see them through lumen/limits.hpp, so
# they are PUBLIC to keep the library and its users in agreement. Header
# defaults: lumen/limits.hpp.
set(LUMEN_LOG_CAPACITY "65536" CACHE STRING "LogBuffer slot count (power of two)")
set(LUMEN_METRIC_CAPACITY "16384" CACHE STRING "MetricBuffer slot count (power of two)")
set(LUMEN_PROGRESS_CAPACITY "4096" CACHE STRING "ProgressBuffer slot count (power of two)")
set(lumen_LOG_CAPACITY_default 65536)
set(lumen_METRIC_CAPACITY_default 16384)
set(lumen_PROGRESS_CAPACITY_default 4096)
foreach(kind IN ITEMS LOG METRIC PROGRESS)
  if(NOT LUMEN_${kind}_CAPACITY EQUAL lumen_${kind}_CAPACITY_default)
    target_compile_definitions(lumen PUBLIC LUMEN_${kind}_CAPACITY=${LUMEN_${kind}_CAPACITY})
  endif()
endforeach()
