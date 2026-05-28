add_executable(lumen_tests
  test_record.cpp
  test_transport.cpp
  test_predicate.cpp
  test_dispatch.cpp
  test_macros.cpp
  test_scope.cpp
  test_tagged.cpp
  test_sinks.cpp
)

target_link_libraries(lumen_tests PRIVATE lumen GTest::GTest GTest::Main)
target_compile_features(lumen_tests PRIVATE cxx_std_20)

include(GoogleTest)
gtest_discover_tests(lumen_tests)
