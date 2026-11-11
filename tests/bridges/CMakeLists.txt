# One test executable per bridge, linked with the real framework. The
# frameworks are found in frameworks.cmake (included by the parent so that the
# compile checks see them too). A bridge whose framework is missing gets one
# ctest entry, <Name>Bridge.SkippedFrameworkNotFound, that reports "Skipped".

add_executable(lumen_bridge_framework_not_found framework_not_found.cpp)

# lumen_bridge_test(<key> <TestPrefix> <libraries>...)
function(lumen_bridge_test key prefix)
  if(lumen_bridge_${key}_found)
    set(target "lumen_tests_bridge_${key}")
    add_executable(${target} test_${key}.cpp)
    target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(${target} PRIVATE lumen GTest::gtest GTest::gtest_main ${ARGN})
    # PRE_TEST: list the tests when ctest runs rather than right after the
    # link, so that a framework library that fails to load is reported as a
    # failed test, not as a broken build.
    gtest_discover_tests(${target} DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 120)
  else()
    set(name "${prefix}.SkippedFrameworkNotFound")
    add_test(NAME ${name}
             COMMAND lumen_bridge_framework_not_found "${lumen_bridge_${key}_skip_reason}")
    set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77)
  endif()
endfunction()

lumen_bridge_test(eigen EigenBridge Eigen3::Eigen)
lumen_bridge_test(openmp OpenMpBridge OpenMP::OpenMP_CXX)
lumen_bridge_test(torch TorchBridge ${TORCH_LIBRARIES})
lumen_bridge_test(root RootBridge ROOT::Tree ROOT::RIO ROOT::Core)
