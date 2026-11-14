# pybind11 is needed by this module only; the core library does not see it.
# 3.0 is the first version with py::classh (smart_holder) and py::native_enum.
find_package(pybind11 3.0 CONFIG REQUIRED)

# co_qualname (the source of records logged from Python) is new in 3.11.
if(Python_VERSION VERSION_LESS 3.11)
  message(FATAL_ERROR "Lumen's Python module needs Python 3.11 or later, found ${Python_VERSION}")
endif()

pybind11_add_module(lumen_bindings
    module.cpp
    errors.cpp
    records.cpp
    predicates.cpp
    tag_values.cpp
    caller_source.cpp
    python_sinks.cpp
    python_diagnostics.cpp
    sinks.cpp
    interpreter_exit.cpp
    core_bindings.cpp
    producers.cpp
    context_bindings.cpp
)

target_link_libraries(lumen_bindings PRIVATE lumen)
target_compile_features(lumen_bindings PRIVATE cxx_std_23)

# The Python tests (tests/python), run by ctest as one test. When the module
# cannot be imported, tests/python/support.py prints "SKIP:" and re-raises the
# ImportError; the line is matched below as well, so the test fails even if a
# runner reports the import error as a skip. The tests that start child
# processes bound each one with a timeout of their own; the ctest timeout is
# the backstop.
if(LUMEN_BUILD_TESTS)
  set(lumen_python_test_env
    "PYTHONPATH=path_list_prepend:$<TARGET_FILE_DIR:lumen_bindings>"
    "PYTHONPATH=path_list_prepend:${CMAKE_CURRENT_SOURCE_DIR}")

  # A sanitized module can only be loaded into the uninstrumented interpreter
  # if the sanitizer runtime is preloaded. The interpreter's own allocations
  # are not ours to check, hence no leak detection.
  set(lumen_python_test_supported TRUE)
  if(NOT LUMEN_SANITIZER STREQUAL "none")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
      if(LUMEN_SANITIZER STREQUAL "address")
        set(lumen_sanitizer_runtime libasan.so)
        # PYTHONMALLOC=malloc: Python objects come from malloc, not from
        # Python's own pools, so ASan sees a use of a freed Python object
        # (a str whose text the module still points into, for instance).
        list(APPEND lumen_python_test_env "ASAN_OPTIONS=set:detect_leaks=0"
                                          "PYTHONMALLOC=set:malloc")
      else()
        set(lumen_sanitizer_runtime libtsan.so)
        # The fork tests start threads in the child of a multithreaded fork,
        # which ThreadSanitizer refuses by default.
        list(APPEND lumen_python_test_env "TSAN_OPTIONS=set:die_after_fork=0")
      endif()
      execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}" -print-file-name=${lumen_sanitizer_runtime}
        OUTPUT_VARIABLE lumen_sanitizer_runtime_path
        OUTPUT_STRIP_TRAILING_WHITESPACE)
      set(lumen_preload "${lumen_sanitizer_runtime_path}")
      if(LUMEN_SANITIZER STREQUAL "address" AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        # ASan intercepts __cxa_throw, and finds the real one only if the C++
        # runtime is loaded when ASan starts. The interpreter does not link
        # it, so without this the first exception the module throws (every
        # Python exception it raises) stops the process.
        execute_process(
          COMMAND "${CMAKE_CXX_COMPILER}" -print-file-name=libstdc++.so
          OUTPUT_VARIABLE lumen_cxx_runtime_path
          OUTPUT_STRIP_TRAILING_WHITESPACE)
        string(APPEND lumen_preload ":${lumen_cxx_runtime_path}")
      endif()
      list(APPEND lumen_python_test_env "LD_PRELOAD=set:${lumen_preload}")
    else()
      set(lumen_python_test_supported FALSE)
      message(STATUS "Lumen: the Python test needs LUMEN_SANITIZER=none on this platform; not registered")
    endif()
  endif()

  if(lumen_python_test_supported)
    add_test(NAME Python.Bindings
      COMMAND "${Python_EXECUTABLE}" -m unittest discover
              -s "${PROJECT_SOURCE_DIR}/tests/python" -t "${PROJECT_SOURCE_DIR}/tests/python" -v)
    set_tests_properties(Python.Bindings PROPERTIES
      ENVIRONMENT_MODIFICATION "${lumen_python_test_env}"
      FAIL_REGULAR_EXPRESSION "SKIP:"
      TIMEOUT 600)
    # The Python example and the documentation's Python blocks, in the same
    # environment.
    include("${PROJECT_SOURCE_DIR}/tests/docs/python_tests.cmake")
  endif()
endif()
