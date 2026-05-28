cmake_minimum_required(VERSION 3.28)
project(lumen VERSION 0.1.0 LANGUAGES CXX)

# ── C++ Standard ──────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Build types ───────────────────────────────────────────────────────────────
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Warnings as errors ────────────────────────────────────────────────────────
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic -Werror)
  add_compile_options(-Wno-unknown-warning-option)
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  add_compile_options(/W4 /WX)
endif()

# ── Sanitizers (Debug) ────────────────────────────────────────────────────────
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
  endif()
endif()

# ── Platform macros ───────────────────────────────────────────────────────────
if(WIN32)
  add_compile_definitions(PLATFORM_WINDOWS)
elseif(APPLE)
  add_compile_definitions(PLATFORM_MACOS)
else()
  add_compile_definitions(PLATFORM_LINUX)
endif()

# ── Options ───────────────────────────────────────────────────────────────────
option(LUMEN_ENABLE_DASHBOARD  "Include FTXUI dependency; enables dashboard rendering" ON)
option(LUMEN_ENABLE_LATEX     "Include MicroTeX; enables LaTeX math rendering in sinks" OFF)
option(LUMEN_ENABLE_PYTHON    "Build pybind11 bindings" OFF)
set(LUMEN_ENABLE_REFLECTION "AUTO" CACHE STRING "Enable C++26 class-name harvesting (AUTO, ON, OFF)")

# ── vcpkg ─────────────────────────────────────────────────────────────────────
# CMAKE_TOOLCHAIN_FILE is set via CMakePresets.json using $env{VCPKG_ROOT}.
# Dependencies are declared in vcpkg.json (manifest mode).

# ── Library target ────────────────────────────────────────────────────────────
add_library(lumen
  src/core.cpp
  src/sink.cpp
  src/predicate.cpp
  src/context.cpp
  src/tagged.cpp
  src/error.cpp
)

target_include_directories(lumen PUBLIC include)
target_include_directories(lumen PRIVATE src)

target_compile_features(lumen PUBLIC cxx_std_23)

# Compile-time level threshold
set(LUMEN_MIN_LEVEL "DEBUG" CACHE STRING "Elides records below this level at compile time")
target_compile_definitions(lumen PUBLIC LUMEN_MIN_LEVEL=${LUMEN_MIN_LEVEL})

# Arena size
set(LUMEN_ARENA_SIZE_KB "4" CACHE STRING "Per-thread arena size in KB")
target_compile_definitions(lumen PRIVATE LUMEN_ARENA_SIZE_KB=${LUMEN_ARENA_SIZE_KB})

# Buffer capacities
set(LUMEN_LOG_CAPACITY "65536" CACHE STRING "LogBuffer slot count (must be power of two)")
set(LUMEN_METRIC_CAPACITY "16384" CACHE STRING "MetricBuffer slot count")
set(LUMEN_PROGRESS_CAPACITY "4096" CACHE STRING "ProgressBuffer slot count")

target_compile_definitions(lumen PRIVATE
  LUMEN_LOG_CAPACITY=${LUMEN_LOG_CAPACITY}
  LUMEN_METRIC_CAPACITY=${LUMEN_METRIC_CAPACITY}
  LUMEN_PROGRESS_CAPACITY=${LUMEN_PROGRESS_CAPACITY}
)

# Feature flags
if(LUMEN_ENABLE_DASHBOARD)
  target_compile_definitions(lumen PRIVATE LUMEN_ENABLE_DASHBOARD)
  find_package(ftxui CONFIG REQUIRED)
  target_link_libraries(lumen PRIVATE ftxui::dom ftxui::screen)
endif()

if(LUMEN_ENABLE_LATEX)
  target_compile_definitions(lumen PRIVATE LUMEN_ENABLE_LATEX)
endif()

if(LUMEN_ENABLE_PYTHON)
  target_compile_definitions(lumen PUBLIC LUMEN_ENABLE_PYTHON)
  find_package(pybind11 REQUIRED)
  target_link_libraries(lumen PUBLIC pybind11::headers)
endif()

# ── C++26 Reflection ───────────────────────────────────────────────────────────
set(LUMEN_HAS_REFLECTION FALSE)

if(LUMEN_ENABLE_REFLECTION STREQUAL "AUTO" OR LUMEN_ENABLE_REFLECTION STREQUAL "ON")
    message(STATUS "Lumen: probing C++26 static reflection support...")

    file(READ "${CMAKE_SOURCE_DIR}/cmake/try_reflection.cpp" __lumen_reflection_code)

    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_FLAGS "-std=c++26")
    check_cxx_source_compiles("${__lumen_reflection_code}" LUMEN_HAS_REFLECTION)
    set(CMAKE_REQUIRED_FLAGS "")

    if(LUMEN_HAS_REFLECTION)
        message(STATUS "Lumen: C++26 static reflection detected — class-name harvesting enabled")
    elseif(LUMEN_ENABLE_REFLECTION STREQUAL "AUTO")
        message(STATUS "Lumen: C++26 static reflection not available — using function-name parsing fallback")
    elseif(LUMEN_ENABLE_REFLECTION STREQUAL "ON")
        message(SEND_ERROR "LUMEN_ENABLE_REFLECTION=ON but C++26 static reflection is not supported by this compiler"
                           " (requires ^T operator and <meta> header)."
                           " Set to AUTO for automatic fallback or OFF to disable.")
    endif()
endif()

if(LUMEN_HAS_REFLECTION)
    target_compile_definitions(lumen PUBLIC LUMEN_HAS_REFLECTION)
endif()

# ── Aliases for convenience ───────────────────────────────────────────────────
add_library(lumen::lumen ALIAS lumen)

# ── Tests ─────────────────────────────────────────────────────────────────────
option(LUMEN_BUILD_TESTS "Build tests" ON)
if(LUMEN_BUILD_TESTS)
  enable_testing()
  find_package(GTest REQUIRED)
  add_subdirectory(tests)
endif()

# ── Examples ──────────────────────────────────────────────────────────────────
option(LUMEN_BUILD_EXAMPLES "Build examples" ON)
if(LUMEN_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

# ── Python bindings ───────────────────────────────────────────────────────────
if(LUMEN_ENABLE_PYTHON)
  add_subdirectory(python)
endif()
