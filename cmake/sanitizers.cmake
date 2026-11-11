# LUMEN_SANITIZER selects one runtime checker for everything this project
# compiles: none, address (ASan + UBSan) or thread (TSan). ASan and TSan cannot
# be combined in one binary, hence a single choice rather than several options.
#
# Which configuration it instruments:
#   - single-configuration generators (Ninja, Unix Makefiles): the one build,
#     whatever CMAKE_BUILD_TYPE is;
#   - multi-configuration generators (Ninja Multi-Config, Visual Studio,
#     Xcode): the Debug configuration only. Release, RelWithDebInfo and
#     MinSizeRel are never instrumented.
#
# The default is address for a top-level build that has a Debug
# configuration (CMAKE_BUILD_TYPE=Debug, or any multi-configuration
# generator) and none otherwise: an instrumented static library forces every
# consumer to link the sanitizer runtime too, so a parent project never gets
# one it did not ask for.

get_property(lumen_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(PROJECT_IS_TOP_LEVEL AND (lumen_multi_config OR CMAKE_BUILD_TYPE STREQUAL "Debug"))
  set(lumen_sanitizer_default address)
else()
  set(lumen_sanitizer_default none)
endif()
set(LUMEN_SANITIZER "${lumen_sanitizer_default}" CACHE STRING
    "Runtime checker for this build: none, address (ASan + UBSan) or thread (TSan)")
set_property(CACHE LUMEN_SANITIZER PROPERTY STRINGS none address thread)

if(DEFINED LUMEN_ENABLE_SANITIZERS)
  message(WARNING "LUMEN_ENABLE_SANITIZERS is no longer read; use LUMEN_SANITIZER=none|address|thread")
endif()

if(LUMEN_SANITIZER STREQUAL "none")
  return()
endif()

if(NOT LUMEN_SANITIZER MATCHES "^(address|thread)$")
  message(FATAL_ERROR "LUMEN_SANITIZER='${LUMEN_SANITIZER}' is not one of: none, address, thread")
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  if(LUMEN_SANITIZER STREQUAL "address")
    # -fno-sanitize-recover: a UBSan finding fails the test instead of only
    # printing a line that a passing run hides.
    set(lumen_sanitizer_flags -fsanitize=address,undefined -fno-sanitize-recover=undefined)
  else()
    set(lumen_sanitizer_flags -fsanitize=thread)
  endif()
  set(lumen_sanitizer_compile_flags ${lumen_sanitizer_flags} -fno-omit-frame-pointer)
  set(lumen_sanitizer_link_flags ${lumen_sanitizer_flags})
elseif(MSVC AND LUMEN_SANITIZER STREQUAL "address")
  set(lumen_sanitizer_compile_flags /fsanitize=address)
  set(lumen_sanitizer_link_flags "")
else()
  message(FATAL_ERROR "LUMEN_SANITIZER=${LUMEN_SANITIZER} is not supported with ${CMAKE_CXX_COMPILER_ID}")
endif()

if(lumen_multi_config)
  list(TRANSFORM lumen_sanitizer_compile_flags REPLACE "^(.+)$" "$<$<CONFIG:Debug>:\\1>")
  list(TRANSFORM lumen_sanitizer_link_flags REPLACE "^(.+)$" "$<$<CONFIG:Debug>:\\1>")
  message(STATUS "Lumen: sanitizer ${LUMEN_SANITIZER} (Debug configuration only)")
else()
  message(STATUS "Lumen: sanitizer ${LUMEN_SANITIZER}")
endif()
add_compile_options(${lumen_sanitizer_compile_flags})
add_link_options(${lumen_sanitizer_link_flags})
