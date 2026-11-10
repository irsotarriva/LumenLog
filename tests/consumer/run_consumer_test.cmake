# Proves that Lumen can be consumed by another CMake project, in both ways a
# user would do it. Run as `cmake -P` by the ctest test Consumer.*:
#   1. configure, build and install Lumen into a fresh prefix;
#   2. build tests/consumer with find_package(lumen) against that prefix, check
#      that its compile command carries no Lumen definition, and run it;
#   3. build tests/consumer with add_subdirectory(<source tree>) and run it.
#
# Inputs (-D): LUMEN_SOURCE_DIR, WORK_DIR, GENERATOR, CONFIG, CXX_COMPILER,
# CXX_FLAGS, EXE_LINKER_FLAGS, MAKE_PROGRAM, EXE_SUFFIX, ENABLE_DASHBOARD,
# FTXUI_DIR (only when ENABLE_DASHBOARD is on).

foreach(var IN ITEMS LUMEN_SOURCE_DIR WORK_DIR GENERATOR CONFIG CXX_COMPILER)
  if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
    message(FATAL_ERROR "run_consumer_test.cmake: ${var} is required")
  endif()
endforeach()

set(prefix "${WORK_DIR}/prefix")
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

# Settings every configure below shares: same compiler, flags and generator as
# the enclosing build, no sanitizer, and FTXUI from wherever the enclosing
# build found it.
set(common_args
  -G "${GENERATOR}"
  "-DCMAKE_BUILD_TYPE=${CONFIG}"
  "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}"
  "-DCMAKE_CXX_FLAGS=${CXX_FLAGS}"
  "-DCMAKE_EXE_LINKER_FLAGS=${EXE_LINKER_FLAGS}"
  -DLUMEN_SANITIZER=none
  "-DLUMEN_ENABLE_DASHBOARD=${ENABLE_DASHBOARD}"
)
if(MAKE_PROGRAM)
  list(APPEND common_args "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}")
endif()
if(ENABLE_DASHBOARD)
  list(APPEND common_args "-Dftxui_DIR=${FTXUI_DIR}")
endif()

function(run_step description)
  message(STATUS "consumer test: ${description}")
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE result)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "consumer test: '${description}' failed (${result})")
  endif()
endfunction()

# Fails if the compile command of the consumer's own source defines any
# LUMEN_* or PLATFORM_* macro: a default build must need none.
function(check_no_lumen_definitions build_dir)
  set(db "${build_dir}/compile_commands.json")
  if(NOT EXISTS "${db}")
    message(STATUS "consumer test: no compile_commands.json (generator ${GENERATOR}); definition check skipped")
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

function(build_and_run_consumer mode)
  set(build_dir "${WORK_DIR}/consumer-${mode}")
  run_step("configure consumer (${mode})"
    "${CMAKE_COMMAND}" -S "${LUMEN_SOURCE_DIR}/tests/consumer" -B "${build_dir}"
    ${common_args} ${ARGN} "-DLUMEN_CONSUMER_MODE=${mode}")
  run_step("build consumer (${mode})"
    "${CMAKE_COMMAND}" --build "${build_dir}" --config "${CONFIG}" --parallel)
  if(mode STREQUAL "package")
    check_no_lumen_definitions("${build_dir}")
  endif()
  run_step("run consumer (${mode})" "${build_dir}/bin/lumen_consumer${EXE_SUFFIX}")
endfunction()

run_step("configure Lumen"
  "${CMAKE_COMMAND}" -S "${LUMEN_SOURCE_DIR}" -B "${WORK_DIR}/lumen-build" ${common_args}
  -DLUMEN_BUILD_TESTS=OFF -DLUMEN_BUILD_EXAMPLES=OFF -DLUMEN_INSTALL=ON
  "-DCMAKE_INSTALL_PREFIX=${prefix}")
run_step("build Lumen"
  "${CMAKE_COMMAND}" --build "${WORK_DIR}/lumen-build" --config "${CONFIG}" --parallel)
run_step("install Lumen"
  "${CMAKE_COMMAND}" --install "${WORK_DIR}/lumen-build" --config "${CONFIG}")

build_and_run_consumer(package "-DCMAKE_PREFIX_PATH=${prefix}")
build_and_run_consumer(subdirectory "-DLUMEN_SOURCE_DIR=${LUMEN_SOURCE_DIR}")
