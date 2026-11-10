add_executable(lumen_tests
  test_record.cpp
  test_transport.cpp
  test_predicate.cpp
  test_predicate_parser.cpp
  test_dispatch.cpp
  test_macros.cpp
  test_scope.cpp
  test_tagged.cpp
  test_sinks.cpp
  test_dashboard.cpp
  test_latex.cpp
  test_error.cpp
  test_reflection.cpp
)

if(LUMEN_ENABLE_DASHBOARD)
  target_compile_definitions(lumen_tests PRIVATE LUMEN_ENABLE_DASHBOARD)
endif()

if(LUMEN_ENABLE_LATEX)
  target_compile_definitions(lumen_tests PRIVATE LUMEN_ENABLE_LATEX)
endif()

target_link_libraries(lumen_tests PRIVATE lumen GTest::gtest GTest::gtest_main)
target_compile_features(lumen_tests PRIVATE cxx_std_23)

include(GoogleTest)
gtest_discover_tests(lumen_tests)

# Ring buffer throughput. Built with the tests so that it keeps compiling, but
# not registered with ctest: it prints measurements and has nothing to pass or
# fail. Run it from a Release build (see the comment at the top of the file).
add_executable(lumen_bench_transport
  bench_transport.cpp
)

target_link_libraries(lumen_bench_transport PRIVATE lumen)
target_compile_features(lumen_bench_transport PRIVATE cxx_std_23)

add_subdirectory(compile_checks)
