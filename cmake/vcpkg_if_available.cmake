# Toolchain file used by CMakePresets.json. It loads the vcpkg toolchain when
# the VCPKG_ROOT environment variable names a vcpkg checkout, and does nothing
# otherwise, so that the same presets work with vcpkg (CI) and with
# system-installed packages (GTest, FTXUI, pybind11 found via CMAKE_PREFIX_PATH).
if(DEFINED ENV{VCPKG_ROOT} AND EXISTS "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
  include("$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
endif()
