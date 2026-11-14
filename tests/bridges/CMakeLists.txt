# One test executable per bridge, linked with the real framework. The
# frameworks are found in frameworks.cmake (included by the parent so that the
# compile checks see them too). A bridge whose framework is missing has no
# test at all, so that a passing run never contains a bridge test that did
# not run; frameworks.cmake lists the missing ones in the configure output.

# lumen_bridge_test(<key> <libraries>...)
function(lumen_bridge_test key)
  if(NOT lumen_bridge_${key}_found)
    return()
  endif()
  set(target "lumen_tests_bridge_${key}")
  add_executable(${target} test_${key}.cpp)
  target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/tests")
  target_link_libraries(${target} PRIVATE lumen GTest::gtest GTest::gtest_main ${ARGN})
  # PRE_TEST: list the tests when ctest runs rather than right after the
  # link, so that a framework library that fails to load is reported as a
  # failed test, not as a broken build.
  gtest_discover_tests(${target} DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 120)
endfunction()

lumen_bridge_test(eigen Eigen3::Eigen)
lumen_bridge_test(openmp OpenMP::OpenMP_CXX)
lumen_bridge_test(torch ${TORCH_LIBRARIES})
lumen_bridge_test(root ROOT::Tree ROOT::RIO ROOT::Core)
