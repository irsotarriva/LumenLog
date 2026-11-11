# The frameworks the bridges in include/lumen/bridges wrap. None of them is a
# dependency of Lumen: they are looked for here, only for the tests, and never
# downloaded. Point CMake at an installation with CMAKE_PREFIX_PATH (or
# Torch_DIR, ROOT_DIR, Eigen3_DIR).
#
# For each framework this sets lumen_bridge_<key>_found (TRUE or FALSE) and,
# when it is FALSE, lumen_bridge_<key>_skip_reason; <key> is eigen, openmp,
# torch or root. A missing framework is reported in the configure log, and its
# bridge tests become one ctest entry that reports itself as skipped.
#
# LUMEN_REQUIRED_BRIDGE_FRAMEWORKS turns a missing framework into a configure
# error, so that a CI job meant to test a bridge cannot pass without it.

set(LUMEN_REQUIRED_BRIDGE_FRAMEWORKS "" CACHE STRING
    "Bridge frameworks that must be found (any of: Eigen3;OpenMP;Torch;ROOT); configuring fails otherwise")

set(lumen_bridge_frameworks Eigen3 OpenMP Torch ROOT)
foreach(framework IN LISTS LUMEN_REQUIRED_BRIDGE_FRAMEWORKS)
  if(NOT framework IN_LIST lumen_bridge_frameworks)
    message(FATAL_ERROR "Lumen: unknown framework '${framework}' in LUMEN_REQUIRED_BRIDGE_FRAMEWORKS "
                        "(expected any of: ${lumen_bridge_frameworks})")
  endif()
endforeach()

find_package(Eigen3 3.3 CONFIG QUIET)
find_package(OpenMP QUIET COMPONENTS CXX)
find_package(Torch CONFIG QUIET)
find_package(ROOT CONFIG QUIET COMPONENTS Tree RIO)

set(lumen_bridge_eigen_found ${Eigen3_FOUND})
set(lumen_bridge_eigen_version "Eigen ${Eigen3_VERSION}")
set(lumen_bridge_eigen_skip_reason "Eigen3 was not found")

set(lumen_bridge_openmp_found ${OpenMP_CXX_FOUND})
set(lumen_bridge_openmp_version "OpenMP ${OpenMP_CXX_VERSION}")
set(lumen_bridge_openmp_skip_reason "OpenMP for C++ was not found")
if(lumen_bridge_openmp_found AND OpenMP_CXX_VERSION VERSION_LESS 3.0)
  set(lumen_bridge_openmp_found FALSE)
  set(lumen_bridge_openmp_skip_reason
      "OpenMP ${OpenMP_CXX_VERSION} is older than 3.0, which the bridge needs (omp_get_level)")
endif()
# The OpenMP runtime is not built with ThreadSanitizer, which then cannot see
# the synchronisation in its barriers and reports races that are not there.
if(lumen_bridge_openmp_found AND LUMEN_SANITIZER STREQUAL "thread")
  set(lumen_bridge_openmp_found FALSE)
  set(lumen_bridge_openmp_skip_reason "the OpenMP runtime is not instrumented for ThreadSanitizer")
endif()

set(lumen_bridge_torch_found ${Torch_FOUND})
set(lumen_bridge_torch_version "libtorch ${Torch_VERSION}")
set(lumen_bridge_torch_skip_reason "libtorch (find_package(Torch)) was not found")

set(lumen_bridge_root_found ${ROOT_FOUND})
set(lumen_bridge_root_version "ROOT ${ROOT_VERSION}")
set(lumen_bridge_root_skip_reason "ROOT (find_package(ROOT)) was not found")

foreach(framework IN LISTS lumen_bridge_frameworks)
  string(TOLOWER "${framework}" key)
  string(REGEX REPLACE "3$" "" key "${key}")
  if(lumen_bridge_${key}_found)
    set(lumen_bridge_${key}_found TRUE)
    message(STATUS "Lumen: ${framework} bridge tests: built (${lumen_bridge_${key}_version})")
  else()
    set(lumen_bridge_${key}_found FALSE)
    message(STATUS "Lumen: ${framework} bridge tests: SKIPPED, ${lumen_bridge_${key}_skip_reason}")
    if(framework IN_LIST LUMEN_REQUIRED_BRIDGE_FRAMEWORKS)
      message(FATAL_ERROR "Lumen: LUMEN_REQUIRED_BRIDGE_FRAMEWORKS lists ${framework}, but "
                          "${lumen_bridge_${key}_skip_reason}")
    endif()
  endif()
endforeach()
