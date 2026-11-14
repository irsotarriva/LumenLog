# The example programs. Each one is also a test: ctest runs it and compares
# what it writes with the files in expected/ (tests/docs/compare_output.py
# says which parts of a line are masked, such as times and thread ids). The
# documentation quotes the examples and the expected files, and
# tests/docs/check_docs.py checks that the quotes match.

set(lumen_examples quick_start context_layers routing custom_sink)
foreach(example IN LISTS lumen_examples)
  add_executable(lumen_example_${example} ${example}.cpp)
  target_link_libraries(lumen_example_${example} PRIVATE lumen)
endforeach()

if(NOT LUMEN_BUILD_TESTS)
  return()
endif()

find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(lumen_compare "${PROJECT_SOURCE_DIR}/tests/docs/compare_output.py")
set(lumen_expected "${CMAKE_CURRENT_SOURCE_DIR}/expected")

add_test(NAME Example.QuickStart
  COMMAND Python3::Interpreter "${lumen_compare}"
          --source-dir "${PROJECT_SOURCE_DIR}"
          --expect-stderr "${lumen_expected}/quick_start.stderr"
          -- $<TARGET_FILE:lumen_example_quick_start>)

add_test(NAME Example.ContextLayers
  COMMAND Python3::Interpreter "${lumen_compare}"
          --source-dir "${PROJECT_SOURCE_DIR}"
          --expect-stderr "${lumen_expected}/context_layers.stderr"
          -- $<TARGET_FILE:lumen_example_context_layers>)

set(lumen_routing_dir "${CMAKE_CURRENT_BINARY_DIR}/routing_output")
file(MAKE_DIRECTORY "${lumen_routing_dir}")
add_test(NAME Example.Routing
  COMMAND Python3::Interpreter "${lumen_compare}"
          --source-dir "${PROJECT_SOURCE_DIR}"
          --expect-stdout "${lumen_expected}/routing.stdout"
          --expect-stderr "${lumen_expected}/routing.stderr"
          --expect-file "${lumen_routing_dir}/routing.log=${lumen_expected}/routing.log"
          --expect-file "${lumen_routing_dir}/routing.logfmt=${lumen_expected}/routing.logfmt"
          --expect-file "${lumen_routing_dir}/routing.jsonl=${lumen_expected}/routing.jsonl"
          -- $<TARGET_FILE:lumen_example_routing> "${lumen_routing_dir}")

add_test(NAME Example.CustomSink
  COMMAND Python3::Interpreter "${lumen_compare}"
          --source-dir "${PROJECT_SOURCE_DIR}"
          --expect-stdout "${lumen_expected}/custom_sink.stdout"
          -- $<TARGET_FILE:lumen_example_custom_sink>)

set_tests_properties(Example.QuickStart Example.ContextLayers Example.Routing Example.CustomSink
                     PROPERTIES TIMEOUT 60 LABELS example)

# The Python example (python/quick_start.py) is registered in
# tests/docs/python_tests.cmake, next to the module's own tests, because it
# needs the same test environment.
