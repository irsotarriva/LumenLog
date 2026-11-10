# Registers the consumer test (see run_consumer_test.cmake). Included from
# tests/CMakeLists.txt.
if(LUMEN_ENABLE_DASHBOARD)
  set(lumen_consumer_ftxui_dir "${ftxui_DIR}")
else()
  set(lumen_consumer_ftxui_dir "")
endif()

add_test(NAME Consumer.FindPackageAndAddSubdirectory
  COMMAND "${CMAKE_COMMAND}"
    "-DLUMEN_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
    "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/consumer"
    "-DGENERATOR=${CMAKE_GENERATOR}"
    "-DCONFIG=$<CONFIG>"
    "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
    "-DCXX_FLAGS=${CMAKE_CXX_FLAGS}"
    "-DEXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}"
    "-DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
    "-DEXE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}"
    "-DENABLE_DASHBOARD=${LUMEN_ENABLE_DASHBOARD}"
    "-DFTXUI_DIR=${lumen_consumer_ftxui_dir}"
    -P "${CMAKE_CURRENT_LIST_DIR}/run_consumer_test.cmake")
# Builds Lumen twice; give it room on slow runners.
set_tests_properties(Consumer.FindPackageAndAddSubdirectory PROPERTIES TIMEOUT 900 LABELS consumer)
