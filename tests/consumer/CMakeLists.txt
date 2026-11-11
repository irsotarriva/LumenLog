# A minimal project that uses Lumen the way a user would. It is configured
# by run_consumer_test.cmake in one of these modes:
#   LUMEN_CONSUMER_MODE=package       find_package(lumen) against an install prefix
#   LUMEN_CONSUMER_MODE=subdirectory  add_subdirectory(<lumen source tree>)
#   LUMEN_CONSUMER_MODE=plain         no CMake package: only the install's include
#                                     directory and its library file, as a
#                                     Makefile or another build system would use
cmake_minimum_required(VERSION 3.28)
project(lumen_consumer LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(LUMEN_CONSUMER_MODE "package" CACHE STRING "package, subdirectory or plain")
# The settings the Lumen under test was built with, which main.cpp checks.
set(CONSUMER_EXPECT_MIN_LEVEL "1" CACHE STRING "LogLevel number of LUMEN_MIN_LEVEL")
set(CONSUMER_EXPECT_LOG_CAPACITY "65536" CACHE STRING "LUMEN_LOG_CAPACITY")
set(CONSUMER_EXPECT_METRIC_CAPACITY "16384" CACHE STRING "LUMEN_METRIC_CAPACITY")
set(CONSUMER_EXPECT_PROGRESS_CAPACITY "4096" CACHE STRING "LUMEN_PROGRESS_CAPACITY")
# plain mode only: one definition for main.cpp, to compile it with a setting
# that disagrees with the library.
set(CONSUMER_EXTRA_DEFINITION "" CACHE STRING "NAME=VALUE added to main.cpp's definitions")

add_executable(lumen_consumer main.cpp)
target_compile_definitions(lumen_consumer PRIVATE
  CONSUMER_EXPECT_MIN_LEVEL=${CONSUMER_EXPECT_MIN_LEVEL}
  CONSUMER_EXPECT_LOG_CAPACITY=${CONSUMER_EXPECT_LOG_CAPACITY}
  CONSUMER_EXPECT_METRIC_CAPACITY=${CONSUMER_EXPECT_METRIC_CAPACITY}
  CONSUMER_EXPECT_PROGRESS_CAPACITY=${CONSUMER_EXPECT_PROGRESS_CAPACITY})
# One fixed path for the driver script to run, whatever the generator; the
# generator expression stops multi-config generators adding a per-config folder.
set_target_properties(lumen_consumer PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin$<0:>")

if(LUMEN_CONSUMER_MODE STREQUAL "package")
  find_package(lumen 0.1 CONFIG REQUIRED)
  # No C++ standard and no definitions are set here: lumen::lumen must carry
  # whatever its headers need.
  target_link_libraries(lumen_consumer PRIVATE lumen::lumen)
elseif(LUMEN_CONSUMER_MODE STREQUAL "subdirectory")
  if(NOT IS_DIRECTORY "${LUMEN_SOURCE_DIR}")
    message(FATAL_ERROR "LUMEN_SOURCE_DIR must name the Lumen source tree")
  endif()
  add_subdirectory("${LUMEN_SOURCE_DIR}" lumen)
  target_link_libraries(lumen_consumer PRIVATE lumen::lumen)
elseif(LUMEN_CONSUMER_MODE STREQUAL "plain")
  # The include directory and the library file of the install; nothing of
  # Lumen's CMake package is read.
  find_path(lumen_include_dir lumen/lumen.hpp PATHS "${LUMEN_PREFIX}/include" NO_DEFAULT_PATH REQUIRED)
  find_library(lumen_library lumen PATHS "${LUMEN_PREFIX}" PATH_SUFFIXES lib lib64
               NO_DEFAULT_PATH REQUIRED)
  find_package(Threads REQUIRED)
  target_compile_features(lumen_consumer PRIVATE cxx_std_23)
  target_include_directories(lumen_consumer PRIVATE "${lumen_include_dir}")
  target_link_libraries(lumen_consumer PRIVATE "${lumen_library}" Threads::Threads)
  if(CONSUMER_EXTRA_DEFINITION)
    target_compile_definitions(lumen_consumer PRIVATE "${CONSUMER_EXTRA_DEFINITION}")
  endif()
else()
  message(FATAL_ERROR "LUMEN_CONSUMER_MODE='${LUMEN_CONSUMER_MODE}' is not package, subdirectory or plain")
endif()
