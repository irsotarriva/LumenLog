add_executable(lumen_tests
  test_record.cpp
  test_record_builder.cpp
  test_transport.cpp
  test_predicate.cpp
  test_predicate_parser.cpp
  test_predicate_lexer.cpp
  test_core.cpp
  test_context.cpp
  test_progress.cpp
  test_macros.cpp
  test_scope.cpp
  test_tagged.cpp
  test_record_format.cpp
  test_terminal_sink.cpp
  test_file_sink.cpp
  test_dashboard_model.cpp
  test_error.cpp
  test_class_name.cpp
)

if(LUMEN_ENABLE_DASHBOARD)
  target_sources(lumen_tests PRIVATE test_dashboard_sink.cpp)
endif()

target_link_libraries(lumen_tests PRIVATE lumen GTest::gtest GTest::gtest_main)
# test_predicate_lexer.cpp tests an internal header of src/.
target_include_directories(lumen_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_features(lumen_tests PRIVATE cxx_std_23)

include(GoogleTest)
# A deadlock shows up as a timeout rather than a hung run.
gtest_discover_tests(lumen_tests PROPERTIES TIMEOUT 120)

# The logging macros at other compile-time thresholds. Each target builds
# test_elision.cpp at one level and prefixes its tests with the level's name.
foreach(level_pair IN ITEMS "Trace;0" "Info;2" "Error;4")
  list(GET level_pair 0 level_name)
  list(GET level_pair 1 level_value)
  string(TOLOWER "${level_name}" level_lower)
  set(target "lumen_tests_elision_${level_lower}")
  add_executable(${target} test_elision.cpp)
  target_compile_definitions(${target} PRIVATE LUMEN_TEST_EXPECTED_MIN_LEVEL=${level_value})
  target_link_libraries(${target} PRIVATE lumen GTest::gtest GTest::gtest_main)
  gtest_discover_tests(${target} TEST_PREFIX "MinLevel${level_name}." PROPERTIES TIMEOUT 120)
endforeach()

# Logging from a static destructor after the global core has shut down. The
# verdict is the process's exit status, so it runs as its own process.
add_executable(lumen_test_late_logging late_logging.cpp)
target_link_libraries(lumen_test_late_logging PRIVATE lumen)
add_test(NAME LateLogging.StaticDestructorLogsAreCountedAfterShutdown
         COMMAND lumen_test_late_logging)
set_tests_properties(LateLogging.StaticDestructorLogsAreCountedAfterShutdown
                     PROPERTIES TIMEOUT 60)

# Logging from destructors that run after the calling thread's context was
# destroyed, while the global core still runs. One process per mode.
add_executable(lumen_test_teardown_logging teardown_logging.cpp)
target_link_libraries(lumen_test_teardown_logging PRIVATE lumen)
foreach(mode IN ITEMS static thread_local)
  set(name "TeardownLogging.RecordsAfterTheThreadContextKeepExplicitAndProcessTags_${mode}")
  add_test(NAME ${name} COMMAND lumen_test_teardown_logging ${mode})
  set_tests_properties(${name} PROPERTIES TIMEOUT 60)
endforeach()

# The Json lines of a FileSink, parsed by a real JSON parser: Python's json
# module, which every CI runner has. No JSON library is added to the build.
add_executable(lumen_json_sample json_sample.cpp)
target_link_libraries(lumen_json_sample PRIVATE lumen)
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_test(NAME JsonOutput.EveryLineParsesStrictlyWithPythonAndHoldsTheExpectedValues
           COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/check_json_lines.py"
                   $<TARGET_FILE:lumen_json_sample>)
  set_tests_properties(JsonOutput.EveryLineParsesStrictlyWithPythonAndHoldsTheExpectedValues
                       PROPERTIES TIMEOUT 60)
else()
  message(STATUS "Lumen: no Python 3 interpreter; the Json output check is not registered")
endif()

# Ring buffer throughput. Built with the tests so that it keeps compiling, but
# not registered with ctest: it prints measurements and has nothing to pass or
# fail. Run it from a Release build (see the comment at the top of the file).
add_executable(lumen_bench_transport
  bench_transport.cpp
)

target_link_libraries(lumen_bench_transport PRIVATE lumen)
target_compile_features(lumen_bench_transport PRIVATE cxx_std_23)

# Optional frameworks for the bridge tests and the bridge compile checks.
include(bridges/frameworks.cmake)
add_subdirectory(bridges)

add_subdirectory(compile_checks)

include(consumer/consumer_test.cmake)

add_subdirectory(docs)
