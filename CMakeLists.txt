cmake_minimum_required(VERSION 3.28)

# vcpkg installs the manifest features while its toolchain file runs inside
# project(), so they must be chosen before it. Only meaningful when Lumen is
# the top-level project and the presets' vcpkg toolchain is in use.
if(NOT DEFINED LUMEN_BUILD_TESTS OR LUMEN_BUILD_TESTS)
  list(APPEND VCPKG_MANIFEST_FEATURES "tests")
endif()
# WIN32 is not known before project(); vcpkg.json itself skips FTXUI there.
if(NOT DEFINED LUMEN_ENABLE_DASHBOARD OR LUMEN_ENABLE_DASHBOARD)
  list(APPEND VCPKG_MANIFEST_FEATURES "dashboard")
endif()
if(LUMEN_ENABLE_PYTHON)
  list(APPEND VCPKG_MANIFEST_FEATURES "python")
endif()

project(lumen VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(GNUInstallDirs)

# Options. When Lumen is built inside another project (add_subdirectory or
# FetchContent), the developer-only settings default to off so that nothing is
# forced on the parent build.
# vcpkg.json does not install FTXUI on Windows, hence the platform default.
if(WIN32)
  set(lumen_dashboard_default OFF)
else()
  set(lumen_dashboard_default ON)
endif()
option(LUMEN_ENABLE_DASHBOARD "Build the FTXUI terminal dashboard" ${lumen_dashboard_default})
option(LUMEN_ENABLE_PYTHON "Build the pybind11 Python module" OFF)
option(LUMEN_BUILD_TESTS "Build the tests" ${PROJECT_IS_TOP_LEVEL})
option(LUMEN_BUILD_EXAMPLES "Build the examples" ${PROJECT_IS_TOP_LEVEL})
option(LUMEN_INSTALL "Generate install rules and the lumen CMake package" ${PROJECT_IS_TOP_LEVEL})
option(LUMEN_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ${PROJECT_IS_TOP_LEVEL})

# Warnings
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Wno-unknown-warning-option)
  endif()
  if(LUMEN_WARNINGS_AS_ERRORS)
    add_compile_options(-Werror)
  endif()
elseif(MSVC)
  add_compile_options(/W4)
  if(LUMEN_WARNINGS_AS_ERRORS)
    add_compile_options(/WX)
  endif()
endif()

include(cmake/sanitizers.cmake)

# Library
add_library(lumen
  src/core.cpp
  src/predicate.cpp
  src/predicate_lexer.cpp
  src/predicate_parser.cpp
  src/context.cpp
  src/tagged.cpp
  src/progress.cpp
  src/error.cpp
  # Sinks, and the line formats and writer thread they share.
  src/text_encoding.cpp
  src/time_format.cpp
  src/record_format.cpp
  src/line_writer.cpp
  src/terminal_output.cpp
  src/terminal_sink.cpp
  src/rotating_file.cpp
  src/file_sink.cpp
)
add_library(lumen::lumen ALIAS lumen)

target_include_directories(lumen
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    src
)
target_compile_features(lumen PUBLIC cxx_std_23)

find_package(Threads REQUIRED)
target_link_libraries(lumen PRIVATE Threads::Threads)

# PRIVATE: only src/ reads the platform macros; no public header does.
if(WIN32)
  target_compile_definitions(lumen PRIVATE PLATFORM_WINDOWS)
elseif(APPLE)
  target_compile_definitions(lumen PRIVATE PLATFORM_MACOS)
else()
  target_compile_definitions(lumen PRIVATE PLATFORM_LINUX)
endif()

include(cmake/build_settings.cmake)

if(LUMEN_ENABLE_DASHBOARD)
  find_package(ftxui CONFIG REQUIRED)
  target_link_libraries(lumen PRIVATE ftxui::dom ftxui::screen)
endif()

if(LUMEN_ENABLE_PYTHON)
  # The static library is linked into the Python extension module, a shared object.
  set_target_properties(lumen PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

if(LUMEN_INSTALL)
  include(cmake/install.cmake)
endif()

if(LUMEN_BUILD_TESTS)
  enable_testing()
  find_package(GTest REQUIRED)
  add_subdirectory(tests)
endif()

if(LUMEN_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(LUMEN_ENABLE_PYTHON)
  add_subdirectory(python)
endif()

include(cmake/lint.cmake)
