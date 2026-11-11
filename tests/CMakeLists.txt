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
  test_text_encoding.cpp
  test_time_format.cpp
  test_line_writer.cpp
  test_terminal_output.cpp
  test_rotating_file.cpp
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
# The tests of the internal pieces (line writer, encodings, time format,
# rotation) include their headers from src/. The platform macros are the
# library's, for the tests that need a pty, a named pipe or TZ.
target_include_directories(lumen_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
if(WIN32)
  target_compile_definitions(lumen_tests PRIVATE PLATFORM_WINDOWS)
elseif(APPLE)
  target_compile_definitions(lumen_tests PRIVATE PLATFORM_MACOS)
else()
  target_compile_definitions(lumen_tests PRIVATE PLATFORM_LINUX)
endif()

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

# exit() from inside a sink callback, on the dispatch thread and inside
# flush(). The verdict is the process's exit status.
add_executable(lumen_test_exit_from_callback exit_from_callback.cpp)
target_link_libraries(lumen_test_exit_from_callback PRIVATE lumen)
foreach(mode IN ITEMS on_log flush)
  add_test(NAME ExitFromCallback.${mode}DeliversWhatIsLeftWithoutAbortOrHang
           COMMAND lumen_test_exit_from_callback ${mode})
  set_tests_properties(ExitFromCallback.${mode}DeliversWhatIsLeftWithoutAbortOrHang
                       PROPERTIES TIMEOUT 60)
endforeach()

# A Core whose dispatch thread cannot be started. The failure is injected by
# replacing pthread_create in the test executable, which only the Linux
# dynamic linker lets the C++ runtime see.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(lumen_tests_core_without_thread core_without_thread.cpp)
  target_link_libraries(lumen_tests_core_without_thread
                        PRIVATE lumen GTest::gtest GTest::gtest_main ${CMAKE_DL_LIBS})
  gtest_discover_tests(lumen_tests_core_without_thread PROPERTIES TIMEOUT 120)
endif()

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
  # The wrapper CI runs ctest through to fail on skipped tests (ci/).
  add_test(NAME CI.CtestWithoutSkips
           COMMAND Python3::Interpreter -B -m unittest -v test_ctest_without_skips
           WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/ci")
  set_tests_properties(CI.CtestWithoutSkips PROPERTIES TIMEOUT 60)
else()
  message(STATUS "Lumen: no Python 3 interpreter; the Json output check is not registered")
endif()

# Standard error on a pipe nobody reads: the sinks must not stall the other
# sinks, and a fatal record must still end the process. POSIX only (pipes as
# standard error, _exit).
if(UNIX)
  add_executable(lumen_blocked_stderr blocked_stderr.cpp)
  target_link_libraries(lumen_blocked_stderr PRIVATE lumen)
  if(Python3_Interpreter_FOUND)
    foreach(mode IN ITEMS deliver fatal)
      add_test(NAME BlockedStderr.${mode}
               COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/check_blocked_stderr.py"
                       $<TARGET_FILE:lumen_blocked_stderr> ${mode})
      set_tests_properties(BlockedStderr.${mode} PROPERTIES TIMEOUT 120)
    endforeach()
  endif()
endif()

# A sink's create() when its thread cannot start. The test replaces
# pthread_create, which works only where the executable's definition takes
# precedence over the C library's: Linux.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(lumen_test_sink_thread_start test_sink_thread_start.cpp)
  target_link_libraries(lumen_test_sink_thread_start
    PRIVATE lumen GTest::gtest GTest::gtest_main ${CMAKE_DL_LIBS})
  target_compile_definitions(lumen_test_sink_thread_start PRIVATE PLATFORM_LINUX
    $<$<BOOL:${LUMEN_ENABLE_DASHBOARD}>:LUMEN_TEST_DASHBOARD>)
  gtest_discover_tests(lumen_test_sink_thread_start PROPERTIES TIMEOUT 120)
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
