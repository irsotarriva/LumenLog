# vcpkg overlay triplet that builds the dependencies against LLVM libc++, so
# that they link into a Lumen build that uses -stdlib=libc++ (the default
# x64-linux triplet builds them against GNU libstdc++). Select the compiler with
# CC/CXX, e.g. CC=clang CXX=clang++.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)
set(VCPKG_CXX_FLAGS "-stdlib=libc++")
set(VCPKG_C_FLAGS "")
set(VCPKG_LINKER_FLAGS "-stdlib=libc++")
