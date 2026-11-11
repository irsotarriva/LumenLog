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

# lumen/config.hpp carries the values CMake was configured with. Other
# settings are built and run by the Consumer.* tests (tests/consumer).
add_executable(lumen_check_config_header config_header.cpp)
target_link_libraries(lumen_check_config_header PRIVATE lumen)
target_compile_definitions(lumen_check_config_header PRIVATE
  LUMEN_CHECK_MIN_LEVEL_VALUE=${lumen_min_level_value}
  LUMEN_CHECK_LOG_CAPACITY=${LUMEN_LOG_CAPACITY}
  LUMEN_CHECK_METRIC_CAPACITY=${LUMEN_METRIC_CAPACITY}
  LUMEN_CHECK_PROGRESS_CAPACITY=${LUMEN_PROGRESS_CAPACITY})
add_test(NAME CompileCheck.ConfigHeaderHoldsTheConfiguredSettings COMMAND lumen_check_config_header)

# macros.hpp alone is enough to expand every logging and scope macro. The
# header-alone check below cannot see this, since it expands no macro.
add_executable(lumen_check_macros_alone macros_alone.cpp)
target_link_libraries(lumen_check_macros_alone PRIVATE lumen)
add_test(NAME CompileCheck.MacrosAlone COMMAND lumen_check_macros_alone)

# Every public header compiles as the only include of a translation unit.
# The bridges are checked separately below.
file(GLOB_RECURSE lumen_public_headers CONFIGURE_DEPENDS
     "${PROJECT_SOURCE_DIR}/include/lumen/*.hpp")
list(FILTER lumen_public_headers EXCLUDE REGEX "/include/lumen/bridges/")
set(lumen_header_tus "")
foreach(header IN LISTS lumen_public_headers)
  file(RELATIVE_PATH rel "${PROJECT_SOURCE_DIR}/include" "${header}")
  string(MAKE_C_IDENTIFIER "${rel}" tu_name)
  set(tu "${CMAKE_CURRENT_BINARY_DIR}/headers/${tu_name}.cpp")
  file(CONFIGURE OUTPUT "${tu}" CONTENT "#include \"${rel}\"\n")
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
