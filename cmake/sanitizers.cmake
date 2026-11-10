# LUMEN_SANITIZER selects one runtime checker for everything this project
# compiles: none, address (ASan + UBSan) or thread (TSan). ASan and TSan cannot
# be combined in one binary, hence a single choice rather than several options.
#
# The default is address for a top-level Debug build and none otherwise: an
# instrumented static library forces every consumer to link the sanitizer
# runtime too, so a parent project never gets one it did not ask for.

if(PROJECT_IS_TOP_LEVEL AND CMAKE_BUILD_TYPE STREQUAL "Debug")
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
  add_compile_options(${lumen_sanitizer_flags} -fno-omit-frame-pointer)
  add_link_options(${lumen_sanitizer_flags})
elseif(MSVC AND LUMEN_SANITIZER STREQUAL "address")
  add_compile_options(/fsanitize=address)
else()
  message(FATAL_ERROR "LUMEN_SANITIZER=${LUMEN_SANITIZER} is not supported with ${CMAKE_CXX_COMPILER_ID}")
endif()

message(STATUS "Lumen: sanitizer ${LUMEN_SANITIZER}")
