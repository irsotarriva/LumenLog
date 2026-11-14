# With a multi-configuration generator, LUMEN_SANITIZER instruments the Debug
# configuration only (cmake/sanitizers.cmake). Run as `cmake -P` by
# Consumer.MultiConfigSanitizesDebugOnly: configures Lumen with Ninja
# Multi-Config, once with the default sanitizer and once with
# LUMEN_SANITIZER=address, and reads the generated build rules of each
# configuration.
#
# Inputs (-D): LUMEN_SOURCE_DIR, WORK_DIR, CXX_COMPILER, MAKE_PROGRAM (ninja).

foreach(var IN ITEMS LUMEN_SOURCE_DIR WORK_DIR CXX_COMPILER MAKE_PROGRAM)
  if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
    message(FATAL_ERROR "multi_config_test.cmake: ${var} is required")
  endif()
endforeach()
file(REMOVE_RECURSE "${WORK_DIR}")

# Fails unless -fsanitize=address is in the Debug rules and in no other.
function(check_rules build_dir)
  foreach(config IN ITEMS Debug Release RelWithDebInfo MinSizeRel)
    file(READ "${build_dir}/CMakeFiles/impl-${config}.ninja" rules)
    string(FIND "${rules}" "-fsanitize=address" at)
    if(config STREQUAL "Debug" AND at EQUAL -1)
      message(FATAL_ERROR "multi-config test: Debug is not built with -fsanitize=address (${build_dir})")
    elseif(NOT config STREQUAL "Debug" AND NOT at EQUAL -1)
      message(FATAL_ERROR "multi-config test: ${config} is built with -fsanitize=address (${build_dir})")
    endif()
  endforeach()
endfunction()

function(configure_and_check case)
  set(build_dir "${WORK_DIR}/${case}")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${LUMEN_SOURCE_DIR}" -B "${build_dir}" -G "Ninja Multi-Config"
            "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}" "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}"
            "-DCMAKE_CONFIGURATION_TYPES=Debug;Release;RelWithDebInfo;MinSizeRel"
            -DLUMEN_BUILD_TESTS=OFF -DLUMEN_BUILD_EXAMPLES=OFF -DLUMEN_ENABLE_DASHBOARD=OFF ${ARGN}
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
  message("${output}")
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "multi-config test: configuring (${case}) failed")
  endif()
  check_rules("${build_dir}")
endfunction()

configure_and_check(default)
configure_and_check(explicit -DLUMEN_SANITIZER=address)
message(STATUS "multi-config test: only Debug is instrumented, by default and with LUMEN_SANITIZER=address")
