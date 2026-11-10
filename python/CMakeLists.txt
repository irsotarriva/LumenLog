# pybind11 is needed by this module only; the core library does not see it.
find_package(pybind11 3.0 CONFIG REQUIRED)

pybind11_add_module(lumen_bindings
    lumen.cpp
)

target_link_libraries(lumen_bindings PRIVATE lumen)
target_compile_features(lumen_bindings PRIVATE cxx_std_23)

# The Python tests, run by ctest. test_python.py prints "SKIP:" and exits 0
# when it cannot import the module; that must count as a failure here.
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
        list(APPEND lumen_python_test_env "ASAN_OPTIONS=set:detect_leaks=0")
      else()
        set(lumen_sanitizer_runtime libtsan.so)
      endif()
      execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}" -print-file-name=${lumen_sanitizer_runtime}
        OUTPUT_VARIABLE lumen_sanitizer_runtime_path
        OUTPUT_STRIP_TRAILING_WHITESPACE)
      list(APPEND lumen_python_test_env "LD_PRELOAD=set:${lumen_sanitizer_runtime_path}")
    else()
      set(lumen_python_test_supported FALSE)
      message(STATUS "Lumen: the Python test needs LUMEN_SANITIZER=none on this platform; not registered")
    endif()
  endif()

  if(lumen_python_test_supported)
    add_test(NAME Python.Bindings
      COMMAND "${Python_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_python.py" -v)
    set_tests_properties(Python.Bindings PROPERTIES
      ENVIRONMENT_MODIFICATION "${lumen_python_test_env}"
      FAIL_REGULAR_EXPRESSION "SKIP:")
  endif()
endif()
