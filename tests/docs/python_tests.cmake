# The Python example and the Python blocks of the documentation. Included from
# python/CMakeLists.txt after Python.Bindings, whose environment
# (lumen_python_test_env: PYTHONPATH, and the sanitizer runtime when the build
# is sanitized) these tests share.

add_test(NAME Example.Python
  COMMAND "${Python_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/docs/compare_output.py"
          --source-dir "${PROJECT_SOURCE_DIR}"
          --expect-stdout "${PROJECT_SOURCE_DIR}/examples/expected/python_quick_start.stdout"
          --expect-stderr "${PROJECT_SOURCE_DIR}/examples/expected/python_quick_start.stderr"
          -- "${Python_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/examples/python/quick_start.py")
set_tests_properties(Example.Python PROPERTIES
  ENVIRONMENT_MODIFICATION "${lumen_python_test_env}"
  TIMEOUT 60 LABELS example)

get_property(lumen_docs_python_blocks GLOBAL PROPERTY LUMEN_DOCS_PYTHON_BLOCKS)
get_property(lumen_docs_blocks_dir GLOBAL PROPERTY LUMEN_DOCS_BLOCKS_DIR)
foreach(block IN LISTS lumen_docs_python_blocks)
  add_test(NAME Docs.Python.${block}
           COMMAND "${Python_EXECUTABLE}" "${lumen_docs_blocks_dir}/${block}.py"
           WORKING_DIRECTORY "${lumen_docs_blocks_dir}")
  set_tests_properties(Docs.Python.${block} PROPERTIES
    ENVIRONMENT_MODIFICATION "${lumen_python_test_env}"
    TIMEOUT 60 LABELS docs)
endforeach()
