# The documentation check (check_docs.py states the rules).
#
# At configure time the script writes every C++ and Python block of README.md,
# AGENTS.md and docs/*.md to ${lumen_docs_out}, with blocks.cmake listing
# them. The C++ blocks are compiled as part of the normal build, with the
# project's warning flags, so a block that stops compiling breaks the build.
# Blocks that are programs are run by ctest. Docs.Check verifies the rest:
# that every block is classified, that quoted examples and outputs equal the
# files they quote, and that the names the documents use exist.

find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(lumen_docs_script "${CMAKE_CURRENT_SOURCE_DIR}/check_docs.py")
set(lumen_docs_out "${CMAKE_CURRENT_BINARY_DIR}/blocks")
set_property(GLOBAL PROPERTY LUMEN_DOCS_BLOCKS_DIR "${lumen_docs_out}")

file(GLOB lumen_docs_inputs CONFIGURE_DEPENDS
     "${PROJECT_SOURCE_DIR}/docs/*.md" "${CMAKE_CURRENT_SOURCE_DIR}/preludes/*.hpp")
list(APPEND lumen_docs_inputs
     "${PROJECT_SOURCE_DIR}/README.md" "${PROJECT_SOURCE_DIR}/AGENTS.md" "${lumen_docs_script}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${lumen_docs_inputs})

execute_process(
  COMMAND "${Python3_EXECUTABLE}" "${lumen_docs_script}" extract "${PROJECT_SOURCE_DIR}" "${lumen_docs_out}"
  RESULT_VARIABLE lumen_docs_result)
if(NOT lumen_docs_result EQUAL 0)
  message(FATAL_ERROR "Lumen: extracting the documentation's code blocks failed")
endif()

# Framework targets of the bridge blocks, as in tests/compile_checks.
set(lumen_docs_libs_eigen Eigen3::Eigen)
set(lumen_docs_libs_openmp OpenMP::OpenMP_CXX)
set(lumen_docs_libs_torch ${TORCH_LIBRARIES})
set(lumen_docs_libs_root ROOT::Tree ROOT::RIO ROOT::Core)
set(lumen_docs_names_eigen Eigen)
set(lumen_docs_names_openmp OpenMp)
set(lumen_docs_names_torch Torch)
set(lumen_docs_names_root Root)

set(lumen_docs_objects "")
foreach(key IN ITEMS eigen openmp torch root)
  set(lumen_docs_objects_${key} "")
endforeach()

# Called by blocks.cmake, once per C++ block.
macro(lumen_docs_cpp_block name where mode requires)
  if(NOT "${requires}" STREQUAL "")
    list(APPEND lumen_docs_objects_${requires} "${lumen_docs_out}/${name}.cpp")
  elseif("${mode}" STREQUAL "program")
    add_executable(lumen_docs_${name} "${lumen_docs_out}/${name}.cpp")
    target_link_libraries(lumen_docs_${name} PRIVATE lumen)
    target_include_directories(lumen_docs_${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME Docs.Run.${name} COMMAND lumen_docs_${name})
    set_tests_properties(Docs.Run.${name} PROPERTIES
      TIMEOUT 60 LABELS docs WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  else()
    list(APPEND lumen_docs_objects "${lumen_docs_out}/${name}.cpp")
  endif()
endmacro()

# Called by blocks.cmake, once per Python block; run in
# tests/docs/python_tests.cmake when the Python module is built.
macro(lumen_docs_python_block name where)
  set_property(GLOBAL APPEND PROPERTY LUMEN_DOCS_PYTHON_BLOCKS "${name}")
endmacro()

set_property(GLOBAL PROPERTY LUMEN_DOCS_PYTHON_BLOCKS "")
include("${lumen_docs_out}/blocks.cmake")

# Fragments are compiled, never linked: they may call functions that the
# preludes only declare.
# stand_ins/ holds headers of made-up libraries that a block uses as an
# example of a third-party framework.
if(lumen_docs_objects)
  add_library(lumen_docs_blocks OBJECT ${lumen_docs_objects})
  target_link_libraries(lumen_docs_blocks PRIVATE lumen)
  target_include_directories(lumen_docs_blocks PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/stand_ins")
endif()

foreach(key IN ITEMS eigen openmp torch root)
  if(NOT lumen_docs_objects_${key})
    continue()
  endif()
  set(test_name "Docs.Bridge${lumen_docs_names_${key}}Blocks.SkippedFrameworkNotFound")
  if(lumen_bridge_${key}_found)
    add_library(lumen_docs_blocks_${key} OBJECT ${lumen_docs_objects_${key}})
    target_link_libraries(lumen_docs_blocks_${key} PRIVATE lumen ${lumen_docs_libs_${key}})
    target_include_directories(lumen_docs_blocks_${key} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  else()
    add_test(NAME ${test_name}
             COMMAND lumen_bridge_framework_not_found "${lumen_bridge_${key}_skip_reason}")
    set_tests_properties(${test_name} PROPERTIES SKIP_RETURN_CODE 77 LABELS docs)
  endif()
endforeach()

add_test(NAME Docs.Check
  COMMAND Python3::Interpreter "${lumen_docs_script}" check "${PROJECT_SOURCE_DIR}" "${lumen_docs_out}")
set_tests_properties(Docs.Check PROPERTIES TIMEOUT 120 LABELS docs)
