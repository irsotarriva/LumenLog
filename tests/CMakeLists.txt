add_executable(lumen_tests
  test_record.cpp
  test_transport.cpp
)

target_link_libraries(lumen_tests PRIVATE lumen GTest::GTest GTest::Main)
target_compile_features(lumen_tests PRIVATE cxx_std_20)

include(GoogleTest)
gtest_discover_tests(lumen_tests)
