# Getting started

## 1. Build and test Lumen

The [README](../README.md#requirements) lists the requirements. On Linux with
GCC 13, CMake 3.28, GoogleTest and Python 3 installed and no vcpkg:

```sh
git clone https://github.com/irsotarriva/LumenLog.git
cd LumenLog
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

The dashboard is built when FTXUI is found and left out otherwise; the
configure output says which. With vcpkg (`VCPKG_ROOT` set), vcpkg installs
FTXUI and GoogleTest.

The `debug` preset builds with AddressSanitizer and UBSan. The test suite
includes the examples (`Example.*`), the documentation checks (`Docs.*`) and
tests that install Lumen and build a small project against it
(`Consumer.*`; `Consumer.FindPackageAndAddSubdirectory` is the slowest one).
The tests that build Lumen with non-default settings are left out of the
`debug`, `tsan`, `release` and `ci` test presets; run them with
`ctest --preset build-settings` after building the `release` preset.

To build only the library, without tests (no GoogleTest or Python needed):

```sh
cmake -S . -B build/lib -DCMAKE_BUILD_TYPE=Release -DLUMEN_BUILD_TESTS=OFF \
      -DLUMEN_BUILD_EXAMPLES=OFF -DLUMEN_ENABLE_DASHBOARD=OFF
cmake --build build/lib
```

## 2. Use it from your project

A project with Lumen checked out (or added as a git submodule) next to its
sources:

<!-- check: illustrative="a directory layout" -->
```text
my_project/
  CMakeLists.txt
  main.cpp
  LumenLog/
```

```cmake
# my_project/CMakeLists.txt
cmake_minimum_required(VERSION 3.28)
project(my_project LANGUAGES CXX)
add_subdirectory(LumenLog)
add_executable(my_program main.cpp)
target_link_libraries(my_program PRIVATE lumen::lumen)
```

`lumen::lumen` brings the include directories and the C++23 requirement;
`LUMEN_MIN_LEVEL` and the queue capacities reach the code through the
generated header `lumen/config.hpp`, not through definitions. Inside another
project, Lumen's tests, examples and install rules are off, its warnings are
not errors, no sanitizer is added, and the dashboard is built only if FTXUI
is found (configuring does not fail without it).

`main.cpp` can be as short as this; ctest builds and runs it:

```cpp
#include <iostream>

#include "lumen/lumen.hpp"

int main() {
    auto sink = lumen::TerminalSink::create({});
    if (!sink) {
        std::cerr << "cannot create the terminal sink: " << sink.error().message() << '\n';
        return 1;
    }
    if (const auto id = lumen::core().add_sink(std::move(*sink)); !id) {
        std::cerr << "cannot register the terminal sink: " << id.error().message() << '\n';
        return 1;
    }
    LUMEN_LOG_INFO("hello from {}", "my_program").tag("stage", "setup");
    lumen::core().flush();
    return 0;
}
```

It prints one line like `12:04:05.123 INFO  hello from my_program` on
standard error (the tag is on the record but not on the line: see
`inline_tags` in [Sinks](sinks.md#terminalsink)).

To use an installed Lumen instead, install a build without sanitizers (a
sanitized static library would force the sanitizer runtime on the consumer):

```sh
cmake --preset release -DLUMEN_ENABLE_DASHBOARD=OFF
cmake --build --preset release
cmake --install build/release --prefix "$HOME/opt/lumen"
```

and in `CMakeLists.txt`, in place of `add_subdirectory`:

```cmake
find_package(lumen 0.1 CONFIG REQUIRED)
```

configured with `-DCMAKE_PREFIX_PATH=$HOME/opt/lumen`. An installed Lumen
built with the dashboard needs FTXUI to be findable by the consumer too.

A build that does not use CMake needs the install's `include` directory and
its library (`lib/liblumen.a`, plus the threads library). The installed
`lumen/config.hpp` holds the settings the library was built with, so such a
build sees the same level threshold and queue capacities; defining another
`LUMEN_LOG_CAPACITY` (or metric or progress capacity) stops the compilation
with an error.

## 3. Next

- [Usage](usage.md): log statements, metrics, progress, context.
- [Sinks](sinks.md): where records go, and writing your own.
- [Predicates](predicates.md): which records go where.
- The examples in `examples/`, each run by ctest.
