# Developer targets, only for a top-level build:
#   lint          clang-tidy (repo .clang-tidy) over every translation unit of
#                 this build in src/, examples/, tests/ and python/: the
#                 dashboard sources when LUMEN_ENABLE_DASHBOARD is on, the
#                 Python module when LUMEN_ENABLE_PYTHON is on
#   format        clang-format -i over every C++ file of the tree, whatever
#                 the options
#   format-check  the same, but only report (fails on any difference)

if(NOT PROJECT_IS_TOP_LEVEL)
  return()
endif()

file(GLOB_RECURSE lumen_format_files CONFIGURE_DEPENDS
  "${PROJECT_SOURCE_DIR}/include/*.hpp"
  "${PROJECT_SOURCE_DIR}/src/*.hpp"
  "${PROJECT_SOURCE_DIR}/src/*.cpp"
  "${PROJECT_SOURCE_DIR}/tests/*.cpp"
  "${PROJECT_SOURCE_DIR}/tests/*.hpp"
  "${PROJECT_SOURCE_DIR}/python/*.cpp"
  "${PROJECT_SOURCE_DIR}/python/*.hpp"
  "${PROJECT_SOURCE_DIR}/examples/*.cpp"
)

find_program(LUMEN_CLANG_FORMAT NAMES clang-format-18 clang-format)
if(LUMEN_CLANG_FORMAT)
  add_custom_target(format
    COMMAND "${LUMEN_CLANG_FORMAT}" -i ${lumen_format_files}
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "clang-format -i"
    VERBATIM)
  add_custom_target(format-check
    COMMAND "${LUMEN_CLANG_FORMAT}" --dry-run --Werror ${lumen_format_files}
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "clang-format --dry-run --Werror"
    VERBATIM)
endif()

find_program(LUMEN_RUN_CLANG_TIDY NAMES run-clang-tidy-18 run-clang-tidy)
find_program(LUMEN_CLANG_TIDY NAMES clang-tidy-18 clang-tidy)
if(LUMEN_RUN_CLANG_TIDY AND LUMEN_CLANG_TIDY)
  # Clang 18 rejects parts of GNU libstdc++ 13 (<expected> is hidden behind a
  # concepts feature-test value that Clang 18 does not yet report). Announcing
  # the C++20 value lets it parse; the redefinition warning that this causes is
  # silenced. Not needed with libc++.
  set(lumen_tidy_extra_args "")
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR
     (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND NOT CMAKE_CXX_FLAGS MATCHES "-stdlib=libc\\+\\+"))
    list(APPEND lumen_tidy_extra_args
      "-extra-arg=-D__cpp_concepts=202002L"
      "-extra-arg=-Wno-builtin-macro-redefined")
  endif()
  # The compile database may hold GCC-only warning flags.
  list(APPEND lumen_tidy_extra_args "-extra-arg=-Wno-unknown-warning-option")

  add_custom_target(lint
    COMMAND "${LUMEN_RUN_CLANG_TIDY}"
            -clang-tidy-binary "${LUMEN_CLANG_TIDY}"
            -p "${PROJECT_BINARY_DIR}"
            -quiet
            "-warnings-as-errors=*"
            ${lumen_tidy_extra_args}
            # The OpenMP bridge check and tests are compiled with GCC's
            # -fopenmp, whose omp.h Clang does not find; the bridge header is
            # linted through the bridges-without-frameworks check instead.
            "^${PROJECT_SOURCE_DIR}/(src|examples|tests|python)/(?!compile_checks/bridge_openmp\\.cpp|bridges/test_openmp\\.cpp)"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "clang-tidy over src/, examples/, tests/ and python/ (dashboard ${LUMEN_ENABLE_DASHBOARD}, Python ${LUMEN_ENABLE_PYTHON})"
    VERBATIM)
endif()
