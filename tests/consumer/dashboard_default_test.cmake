# How LUMEN_ENABLE_DASHBOARD behaves where FTXUI cannot be found
# (cmake/dashboard_option.cmake). Run as `cmake -P` by
# Consumer.DashboardDefaultFollowsFtxui. FTXUI is made unfindable by turning
# off every default search location, so the result is the same on a machine
# that has FTXUI installed:
#   1. left unset, the option is OFF, configuring succeeds and says why;
#   2. set to ON, configuring fails with find_package's error for ftxui.
#
# Inputs (-D): LUMEN_SOURCE_DIR, WORK_DIR, GENERATOR, CXX_COMPILER, MAKE_PROGRAM.

foreach(var IN ITEMS LUMEN_SOURCE_DIR WORK_DIR GENERATOR CXX_COMPILER)
  if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
    message(FATAL_ERROR "dashboard_default_test.cmake: ${var} is required")
  endif()
endforeach()
file(REMOVE_RECURSE "${WORK_DIR}")

set(args
  -G "${GENERATOR}"
  "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}"
  -DLUMEN_BUILD_TESTS=OFF -DLUMEN_BUILD_EXAMPLES=OFF -DLUMEN_SANITIZER=none
  -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF
  -DCMAKE_FIND_USE_CMAKE_SYSTEM_PATH=OFF -DCMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH=OFF
  -DCMAKE_FIND_USE_CMAKE_ENVIRONMENT_PATH=OFF)
if(MAKE_PROGRAM)
  list(APPEND args "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -S "${LUMEN_SOURCE_DIR}" -B "${WORK_DIR}/default" ${args}
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
message("${output}")
if(NOT result EQUAL 0)
  message(FATAL_ERROR "dashboard default test: configuring without the option failed")
endif()
if(NOT output MATCHES "FTXUI not found, LUMEN_ENABLE_DASHBOARD defaults to OFF")
  message(FATAL_ERROR "dashboard default test: no status line saying the dashboard is off")
endif()
file(STRINGS "${WORK_DIR}/default/CMakeCache.txt" cached REGEX "^LUMEN_ENABLE_DASHBOARD:")
if(NOT cached STREQUAL "LUMEN_ENABLE_DASHBOARD:BOOL=OFF")
  message(FATAL_ERROR "dashboard default test: the cache holds '${cached}', expected OFF")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -S "${LUMEN_SOURCE_DIR}" -B "${WORK_DIR}/explicit-on" ${args}
          -DLUMEN_ENABLE_DASHBOARD=ON
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
message("${output}")
if(result EQUAL 0 OR NOT output MATCHES "provided by \"ftxui\"")
  message(FATAL_ERROR "dashboard default test: -DLUMEN_ENABLE_DASHBOARD=ON without FTXUI must fail "
                      "in find_package(ftxui); result ${result}")
endif()
message(STATUS "dashboard default test: unset gives OFF, ON without FTXUI fails")
