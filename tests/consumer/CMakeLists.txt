# A minimal project that uses Lumen the way a user would. It is configured
# twice by run_consumer_test.cmake:
#   LUMEN_CONSUMER_MODE=package       find_package(lumen) against an install prefix
#   LUMEN_CONSUMER_MODE=subdirectory  add_subdirectory(<lumen source tree>)
cmake_minimum_required(VERSION 3.28)
project(lumen_consumer LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(LUMEN_CONSUMER_MODE "package" CACHE STRING "package or subdirectory")
if(LUMEN_CONSUMER_MODE STREQUAL "package")
  find_package(lumen 0.1 CONFIG REQUIRED)
elseif(LUMEN_CONSUMER_MODE STREQUAL "subdirectory")
  if(NOT IS_DIRECTORY "${LUMEN_SOURCE_DIR}")
    message(FATAL_ERROR "LUMEN_SOURCE_DIR must name the Lumen source tree")
  endif()
  add_subdirectory("${LUMEN_SOURCE_DIR}" lumen)
else()
  message(FATAL_ERROR "LUMEN_CONSUMER_MODE='${LUMEN_CONSUMER_MODE}' is not package or subdirectory")
endif()

# No C++ standard and no definitions are set here: lumen::lumen must carry
# whatever its headers need.
add_executable(lumen_consumer main.cpp)
target_link_libraries(lumen_consumer PRIVATE lumen::lumen)
# One fixed path for the driver script to run, whatever the generator; the
# generator expression stops multi-config generators adding a per-config folder.
set_target_properties(lumen_consumer PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin$<0:>")
