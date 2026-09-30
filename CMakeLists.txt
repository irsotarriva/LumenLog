cmake_minimum_required(VERSION 3.28)
project(lumen VERSION 0.1.0 LANGUAGES CXX)

# ── C++ Standard ──────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Build types ───────────────────────────────────────────────────────────────
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# When Lumen is built inside another project (add_subdirectory/FetchContent),
# none of the developer-only settings below are forced on the parent build.
option(LUMEN_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ${PROJECT_IS_TOP_LEVEL})
option(LUMEN_ENABLE_SANITIZERS "Build with ASan/UBSan in Debug" ${PROJECT_IS_TOP_LEVEL})

# ── Warnings ──────────────────────────────────────────────────────────────────
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    add_compile_options(-Wno-unknown-warning-option)
  endif()
  if(LUMEN_WARNINGS_AS_ERRORS)
    add_compile_options(-Werror)
  endif()
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  add_compile_options(/W4)
  if(LUMEN_WARNINGS_AS_ERRORS)
    add_compile_options(/WX)
  endif()
endif()

# ── Sanitizers (Debug) ────────────────────────────────────────────────────────
# Instrumenting a static library forces every consumer to link the sanitizer
# runtime too, so this is off by default when Lumen is a subproject.
if(LUMEN_ENABLE_SANITIZERS AND CMAKE_BUILD_TYPE STREQUAL "Debug")
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

# PUBLIC: the capacities set the layout of Core, which consumers see through the
# headers. A PRIVATE define would give the library and its users different layouts.
target_compile_definitions(lumen PUBLIC
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

    # CMAKE_CURRENT_SOURCE_DIR, not CMAKE_SOURCE_DIR: the latter is the parent
    # project's root when Lumen is added with add_subdirectory/FetchContent.
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/cmake/try_reflection.cpp" __lumen_reflection_code)

    include(CheckCXXSourceCompiles)
    include(CMakePushCheckState)
    cmake_push_check_state(RESET)
    set(CMAKE_REQUIRED_FLAGS "-std=c++26")
    # The result variable must not be pre-set: check_cxx_source_compiles skips
    # the probe entirely when the variable is already defined.
    check_cxx_source_compiles("${__lumen_reflection_code}" LUMEN_REFLECTION_PROBE)
    cmake_pop_check_state()
    set(LUMEN_HAS_REFLECTION ${LUMEN_REFLECTION_PROBE})

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
option(LUMEN_BUILD_TESTS "Build tests" ${PROJECT_IS_TOP_LEVEL})
if(LUMEN_BUILD_TESTS)
  enable_testing()
  find_package(GTest REQUIRED)
  add_subdirectory(tests)
endif()

# ── Examples ──────────────────────────────────────────────────────────────────
option(LUMEN_BUILD_EXAMPLES "Build examples" ${PROJECT_IS_TOP_LEVEL})
if(LUMEN_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

# ── Python bindings ───────────────────────────────────────────────────────────
if(LUMEN_ENABLE_PYTHON)
  add_subdirectory(python)
endif()
