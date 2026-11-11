# Proves that Lumen can be consumed by another project with the settings it
# was built with. Run as `cmake -P` by the ctest tests Consumer.* (see
# consumer_test.cmake):
#   1. configure Lumen with SETTINGS, build it and install it into a fresh
#      prefix (skipped when MODES is only subdirectory);
#   2. build tests/consumer in each of MODES and run it; tests/consumer/main.cpp
#      checks the level threshold and queue capacities it sees against EXPECT:
#        package       find_package(lumen) against the prefix; also checks that
#                      the consumer's compile command carries no Lumen definition
#        subdirectory  add_subdirectory(<source tree>) without any LUMEN_*
#                      option, so with every default; also checks the configure
#                      output states how LUMEN_ENABLE_DASHBOARD was chosen
#        plain         only the prefix's include directory and library file,
#                      as a build that does not use CMake would do it
#        mismatch      as plain, with main.cpp compiled with
#                      -DLUMEN_LOG_CAPACITY=65536, which must fail to compile
#                      against a Lumen built with another log capacity
#
# Inputs (-D): LUMEN_SOURCE_DIR, WORK_DIR, GENERATOR, CONFIG, CXX_COMPILER,
# CXX_FLAGS, EXE_LINKER_FLAGS, MAKE_PROGRAM, EXE_SUFFIX, ENABLE_DASHBOARD,
# FTXUI_DIR (only when ENABLE_DASHBOARD is on); SETTINGS and MODES, lists
# separated by "|"; EXPECT, "<level number>|<log>|<metric>|<progress>".

foreach(var IN ITEMS LUMEN_SOURCE_DIR WORK_DIR GENERATOR CONFIG CXX_COMPILER MODES EXPECT)
  if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
    message(FATAL_ERROR "run_consumer_test.cmake: ${var} is required")
  endif()
endforeach()
string(REPLACE "|" ";" settings "${SETTINGS}")
string(REPLACE "|" ";" modes "${MODES}")
string(REPLACE "|" ";" expect "${EXPECT}")
list(LENGTH expect expect_count)
if(NOT expect_count EQUAL 4)
  message(FATAL_ERROR "run_consumer_test.cmake: EXPECT needs 4 values, got '${EXPECT}'")
endif()
list(GET expect 0 expect_level)
list(GET expect 1 expect_log)
list(GET expect 2 expect_metric)
list(GET expect 3 expect_progress)

set(prefix "${WORK_DIR}/prefix")
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

# Settings every configure below shares: same compiler, flags and generator as
# the enclosing build, and no sanitizer.
set(common_args
  -G "${GENERATOR}"
  "-DCMAKE_BUILD_TYPE=${CONFIG}"
  "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}"
  "-DCMAKE_CXX_FLAGS=${CXX_FLAGS}"
  "-DCMAKE_EXE_LINKER_FLAGS=${EXE_LINKER_FLAGS}"
  -DLUMEN_SANITIZER=none
)
if(MAKE_PROGRAM)
  list(APPEND common_args "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}")
endif()
# FTXUI from wherever the enclosing build found it.
set(ftxui_args "")
if(ENABLE_DASHBOARD)
  set(ftxui_args "-Dftxui_DIR=${FTXUI_DIR}")
endif()
set(expect_args
  "-DCONSUMER_EXPECT_MIN_LEVEL=${expect_level}"
  "-DCONSUMER_EXPECT_LOG_CAPACITY=${expect_log}"
  "-DCONSUMER_EXPECT_METRIC_CAPACITY=${expect_metric}"
  "-DCONSUMER_EXPECT_PROGRESS_CAPACITY=${expect_progress}")

# Runs a command, prints its output, and stores the output in `out_var`.
function(run_step description out_var)
  message(STATUS "consumer test: ${description}")
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
  message("${output}")
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "consumer test: '${description}' failed (${result})")
  endif()
  set(${out_var} "${output}" PARENT_SCOPE)
endfunction()

# Fails if the compile command of the consumer's own source defines any
# LUMEN_* or PLATFORM_* macro: the settings must reach it through the
# installed lumen/config.hpp, not through definitions.
function(check_no_lumen_definitions build_dir)
  set(db "${build_dir}/compile_commands.json")
  if(NOT EXISTS "${db}")
    message(STATUS "consumer test: no compile_commands.json (generator ${GENERATOR}); definition check not run")
    return()
  endif()
  file(READ "${db}" json)
  string(JSON count LENGTH "${json}")
  math(EXPR last "${count} - 1")
  set(found FALSE)
  foreach(i RANGE ${last})
    string(JSON file GET "${json}" ${i} file)
    if(file MATCHES "tests/consumer/main\\.cpp$")
      set(found TRUE)
      string(JSON command GET "${json}" ${i} command)
      if(command MATCHES "[-/]D *(LUMEN|PLATFORM)_[A-Z_]*")
        message(FATAL_ERROR "consumer test: the consumer is compiled with ${CMAKE_MATCH_0}:\n${command}")
      endif()
    endif()
  endforeach()
  if(NOT found)
    message(FATAL_ERROR "consumer test: main.cpp not found in ${db}")
  endif()
endfunction()

# The configure output of a default add_subdirectory() says how
# LUMEN_ENABLE_DASHBOARD was chosen; with FTXUI handed over, it must be on.
function(check_dashboard_default output)
  if(ENABLE_DASHBOARD)
    set(pattern "FTXUI [^\n]*found, LUMEN_ENABLE_DASHBOARD defaults to ON")
  else()
    set(pattern "LUMEN_ENABLE_DASHBOARD defaults to (ON|OFF)")
  endif()
  if(NOT output MATCHES "${pattern}")
    message(FATAL_ERROR "consumer test: the subdirectory configure did not print '${pattern}'")
  endif()
endfunction()

function(configure_and_build_consumer mode build_dir out_var)
  run_step("configure consumer (${mode})" configure_output
    "${CMAKE_COMMAND}" -S "${LUMEN_SOURCE_DIR}/tests/consumer" -B "${build_dir}"
    ${common_args} ${expect_args} "-DLUMEN_CONSUMER_MODE=${mode}" ${ARGN})
  run_step("build consumer (${mode})" build_output
    "${CMAKE_COMMAND}" --build "${build_dir}" --config "${CONFIG}" --parallel)
  set(${out_var} "${configure_output}" PARENT_SCOPE)
endfunction()

function(consumer_mode_package)
  set(build_dir "${WORK_DIR}/consumer-package")
  configure_and_build_consumer(package "${build_dir}" output "-DCMAKE_PREFIX_PATH=${prefix}" ${ftxui_args})
  check_no_lumen_definitions("${build_dir}")
  run_step("run consumer (package)" output "${build_dir}/bin/lumen_consumer${EXE_SUFFIX}")
endfunction()

function(consumer_mode_subdirectory)
  set(build_dir "${WORK_DIR}/consumer-subdirectory")
  configure_and_build_consumer(subdirectory "${build_dir}" output
    "-DLUMEN_SOURCE_DIR=${LUMEN_SOURCE_DIR}" ${ftxui_args})
  check_dashboard_default("${output}")
  run_step("run consumer (subdirectory)" output "${build_dir}/bin/lumen_consumer${EXE_SUFFIX}")
endfunction()

function(consumer_mode_plain)
  set(build_dir "${WORK_DIR}/consumer-plain")
  configure_and_build_consumer(plain "${build_dir}" output "-DLUMEN_PREFIX=${prefix}")
  run_step("run consumer (plain)" output "${build_dir}/bin/lumen_consumer${EXE_SUFFIX}")
endfunction()

function(consumer_mode_mismatch)
  set(build_dir "${WORK_DIR}/consumer-mismatch")
  run_step("configure consumer (mismatch)" output
    "${CMAKE_COMMAND}" -S "${LUMEN_SOURCE_DIR}/tests/consumer" -B "${build_dir}"
    ${common_args} ${expect_args} -DLUMEN_CONSUMER_MODE=plain "-DLUMEN_PREFIX=${prefix}"
    -DCONSUMER_EXTRA_DEFINITION=LUMEN_LOG_CAPACITY=65536)
  message(STATUS "consumer test: build consumer (mismatch), which must fail")
  execute_process(COMMAND "${CMAKE_COMMAND}" --build "${build_dir}" --config "${CONFIG}"
                  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
  set(pattern "LUMEN_LOG_CAPACITY differs from the value this Lumen library was built with \\(${expect_log}\\)")
  if(result EQUAL 0 OR NOT output MATCHES "${pattern}")
    message(FATAL_ERROR "consumer test: compiling with another LUMEN_LOG_CAPACITY must fail with "
                        "'${pattern}'; result ${result}, output:\n${output}")
  endif()
endfunction()

if(NOT modes STREQUAL "subdirectory")
  run_step("configure Lumen" output
    "${CMAKE_COMMAND}" -S "${LUMEN_SOURCE_DIR}" -B "${WORK_DIR}/lumen-build" ${common_args}
    "-DLUMEN_ENABLE_DASHBOARD=${ENABLE_DASHBOARD}" ${ftxui_args} ${settings}
    -DLUMEN_BUILD_TESTS=OFF -DLUMEN_BUILD_EXAMPLES=OFF -DLUMEN_INSTALL=ON
    "-DCMAKE_INSTALL_PREFIX=${prefix}")
  run_step("build Lumen" output
    "${CMAKE_COMMAND}" --build "${WORK_DIR}/lumen-build" --config "${CONFIG}" --parallel)
  run_step("install Lumen" output
    "${CMAKE_COMMAND}" --install "${WORK_DIR}/lumen-build" --config "${CONFIG}")
endif()

foreach(mode IN LISTS modes)
  if(NOT COMMAND consumer_mode_${mode})
    message(FATAL_ERROR "run_consumer_test.cmake: unknown mode '${mode}'")
  endif()
  cmake_language(CALL consumer_mode_${mode})
endforeach()
