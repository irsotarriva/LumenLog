# Build-level checks. Each target here exists to fail the build if a header
# stops compiling in a particular setting; the ctest entries run the result so
# that the check also appears in the test report.

# A consumer that defines DEBUG, ERROR and INFO itself (as -DDEBUG builds and
# <windows.h> do) must still be able to use the logging macros.
add_executable(lumen_check_user_level_macros user_level_macros.cpp)
target_link_libraries(lumen_check_user_level_macros PRIVATE lumen)
add_test(NAME CompileCheck.UserDefinedLevelMacros COMMAND lumen_check_user_level_macros)

# With LUMEN_NO_SHORT_MACROS the LOG_* names are free for the consumer.
add_executable(lumen_check_no_short_macros no_short_macros.cpp)
target_link_libraries(lumen_check_no_short_macros PRIVATE lumen)
add_test(NAME CompileCheck.NoShortMacros COMMAND lumen_check_no_short_macros)

# Without LUMEN_MIN_LEVEL_VALUE the header must fall back to Debug.
add_executable(lumen_check_default_min_level default_min_level.cpp)
target_link_libraries(lumen_check_default_min_level PRIVATE lumen)
add_test(NAME CompileCheck.DefaultMinLevel COMMAND lumen_check_default_min_level)

# The CMake defaults of the level threshold and buffer capacities equal the
# header fallbacks, which is what lets a default build omit the definitions.
add_executable(lumen_check_header_defaults header_defaults.cpp)
target_link_libraries(lumen_check_header_defaults PRIVATE lumen)
target_compile_definitions(lumen_check_header_defaults PRIVATE
  LUMEN_CHECK_MIN_LEVEL_VALUE=${lumen_min_level_value_default}
  LUMEN_CHECK_LOG_CAPACITY=${lumen_LOG_CAPACITY_default}
  LUMEN_CHECK_METRIC_CAPACITY=${lumen_METRIC_CAPACITY_default}
  LUMEN_CHECK_PROGRESS_CAPACITY=${lumen_PROGRESS_CAPACITY_default})
add_test(NAME CompileCheck.HeaderDefaultsMatchCMake COMMAND lumen_check_header_defaults)

# macros.hpp alone is enough to expand every logging and scope macro. The
# header-alone check below cannot see this, since it expands no macro.
add_executable(lumen_check_macros_alone macros_alone.cpp)
target_link_libraries(lumen_check_macros_alone PRIVATE lumen)
add_test(NAME CompileCheck.MacrosAlone COMMAND lumen_check_macros_alone)

# Every header under include/lumen/ (the bridges are checked separately
# below) is the first include of its own translation unit, headers/<path>.cpp
# (lumen/detail/context.hpp -> headers/lumen_detail_context.cpp), which then
# uses the names the header documents. Compiling a header alone is not
# enough: a name it uses from a header it forgot to include shows only when
# the name is used. A header without such a file fails the configuration.
file(GLOB_RECURSE lumen_public_headers CONFIGURE_DEPENDS
     "${PROJECT_SOURCE_DIR}/include/lumen/*.hpp")
list(FILTER lumen_public_headers EXCLUDE REGEX "/include/lumen/bridges/")
set(lumen_header_tus "")
foreach(header IN LISTS lumen_public_headers)
  file(RELATIVE_PATH rel "${PROJECT_SOURCE_DIR}/include" "${header}")
  string(REGEX REPLACE "\\.hpp$" "" stem "${rel}")
  string(REPLACE "/" "_" tu_name "${stem}")
  set(tu "${CMAKE_CURRENT_SOURCE_DIR}/headers/${tu_name}.cpp")
  if(NOT EXISTS "${tu}")
    message(FATAL_ERROR "${rel} has no usage check: add tests/compile_checks/headers/${tu_name}.cpp,"
                        " which includes only ${rel} and uses the names it documents")
  endif()
  list(APPEND lumen_header_tus "${tu}")
endforeach()
add_executable(lumen_check_headers_alone check_main.cpp ${lumen_header_tus})
target_link_libraries(lumen_check_headers_alone PRIVATE lumen)
add_test(NAME CompileCheck.EveryHeaderAlone COMMAND lumen_check_headers_alone)

# Every bridge compiles when its framework is absent: the __has_include (or
# _OPENMP) guard must reduce it to an empty translation unit.
file(GLOB lumen_bridge_headers CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/include/lumen/bridges/*.hpp")
set(lumen_bridge_tus "")
foreach(header IN LISTS lumen_bridge_headers)
  get_filename_component(name "${header}" NAME_WE)
  set(tu "${CMAKE_CURRENT_BINARY_DIR}/bridges/bridge_${name}.cpp")
  file(CONFIGURE OUTPUT "${tu}" CONTENT "#include \"lumen/bridges/${name}.hpp\"\n")
  list(APPEND lumen_bridge_tus "${tu}")
endforeach()
# Compiled but never linked: when a framework's headers happen to be on the
# default include path, the guard lets the bridge through, and its objects
# would then need the framework's libraries, which this check does not have.
add_library(lumen_check_bridge_objects OBJECT ${lumen_bridge_tus})
target_link_libraries(lumen_check_bridge_objects PRIVATE lumen)
add_executable(lumen_check_bridges_without_frameworks check_main.cpp)
target_link_libraries(lumen_check_bridges_without_frameworks PRIVATE lumen)
add_dependencies(lumen_check_bridges_without_frameworks lumen_check_bridge_objects)
add_test(NAME CompileCheck.BridgesWithoutFrameworks COMMAND lumen_check_bridges_without_frameworks)

# Every bridge also compiles, as the only include, with its framework
# (found in tests/bridges/frameworks.cmake); a missing framework is reported
# there and leaves its check out.
function(lumen_bridge_compile_check key name)
  if(lumen_bridge_${key}_found)
    add_executable(lumen_check_bridge_${key} check_main.cpp bridge_${key}.cpp)
    target_link_libraries(lumen_check_bridge_${key} PRIVATE lumen ${ARGN})
    add_test(NAME CompileCheck.Bridge${name}WithFramework COMMAND lumen_check_bridge_${key})
  endif()
endfunction()

lumen_bridge_compile_check(eigen Eigen Eigen3::Eigen)
lumen_bridge_compile_check(openmp OpenMp OpenMP::OpenMP_CXX)
lumen_bridge_compile_check(torch Torch ${TORCH_LIBRARIES})
lumen_bridge_compile_check(root Root ROOT::Tree ROOT::RIO ROOT::Core)
