# Registers the tests that configure and build other projects against Lumen,
# or Lumen itself with other options. Included from tests/CMakeLists.txt.
#
# Label consumer: in every test run.
#   Consumer.FindPackageAndAddSubdirectory   default settings; find_package and
#                                            add_subdirectory (run_consumer_test.cmake)
#   Consumer.DashboardDefaultFollowsFtxui    dashboard_default_test.cmake
#   Consumer.MultiConfigSanitizesDebugOnly   multi_config_test.cmake (needs ninja,
#                                            GCC or Clang)
# Label build-settings: each builds and installs Lumen with non-default
# settings, so they are left out of the default test presets and run by the
# `build-settings` test preset (one CI job runs it).
#   Consumer.WarnLevelAndSmallQueues         LUMEN_MIN_LEVEL=WARN and small queues;
#                                            find_package, a build without CMake,
#                                            and a capacity mismatch that must not compile
#   Consumer.FatalLevel                      LUMEN_MIN_LEVEL=FATAL; find_package

if(LUMEN_ENABLE_DASHBOARD)
  set(lumen_consumer_ftxui_dir "${ftxui_DIR}")
else()
  set(lumen_consumer_ftxui_dir "")
endif()

# lumen_consumer_test(<name> <label> <modes> <expect> [<lumen settings>...])
function(lumen_consumer_test name label modes expect)
  string(JOIN "|" settings ${ARGN})
  add_test(NAME Consumer.${name}
    COMMAND "${CMAKE_COMMAND}"
      "-DLUMEN_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
      "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/consumer/${name}"
      "-DGENERATOR=${CMAKE_GENERATOR}"
      "-DCONFIG=$<CONFIG>"
      "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
      "-DCXX_FLAGS=${CMAKE_CXX_FLAGS}"
      "-DEXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}"
      "-DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
      "-DEXE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}"
      "-DENABLE_DASHBOARD=${LUMEN_ENABLE_DASHBOARD}"
      "-DFTXUI_DIR=${lumen_consumer_ftxui_dir}"
      "-DMODES=${modes}"
      "-DEXPECT=${expect}"
      "-DSETTINGS=${settings}"
      -P "${CMAKE_CURRENT_LIST_DIR}/run_consumer_test.cmake")
  # Each builds Lumen at least once; give them room on slow runners.
  set_tests_properties(Consumer.${name} PROPERTIES TIMEOUT 900 LABELS ${label})
endfunction()

lumen_consumer_test(FindPackageAndAddSubdirectory consumer "package|subdirectory" "1|65536|16384|4096")
lumen_consumer_test(WarnLevelAndSmallQueues build-settings "package|plain|mismatch" "3|1024|512|256"
  -DLUMEN_ENABLE_DASHBOARD=OFF -DLUMEN_MIN_LEVEL=WARN
  -DLUMEN_LOG_CAPACITY=1024 -DLUMEN_METRIC_CAPACITY=512 -DLUMEN_PROGRESS_CAPACITY=256)
lumen_consumer_test(FatalLevel build-settings "package" "5|65536|16384|4096"
  -DLUMEN_ENABLE_DASHBOARD=OFF -DLUMEN_MIN_LEVEL=FATAL)

add_test(NAME Consumer.DashboardDefaultFollowsFtxui
  COMMAND "${CMAKE_COMMAND}"
    "-DLUMEN_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
    "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/consumer/DashboardDefault"
    "-DGENERATOR=${CMAKE_GENERATOR}"
    "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
    "-DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
    -P "${CMAKE_CURRENT_LIST_DIR}/dashboard_default_test.cmake")
set_tests_properties(Consumer.DashboardDefaultFollowsFtxui PROPERTIES TIMEOUT 300 LABELS consumer)

find_program(lumen_ninja NAMES ninja ninja-build)
if(lumen_ninja AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_test(NAME Consumer.MultiConfigSanitizesDebugOnly
    COMMAND "${CMAKE_COMMAND}"
      "-DLUMEN_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
      "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/consumer/MultiConfig"
      "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
      "-DMAKE_PROGRAM=${lumen_ninja}"
      -P "${CMAKE_CURRENT_LIST_DIR}/multi_config_test.cmake")
  set_tests_properties(Consumer.MultiConfigSanitizesDebugOnly PROPERTIES TIMEOUT 300 LABELS consumer)
else()
  message(STATUS "Lumen: Consumer.MultiConfigSanitizesDebugOnly not registered: it needs ninja and GCC or Clang")
endif()
