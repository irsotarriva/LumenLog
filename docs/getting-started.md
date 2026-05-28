# Getting Started

## Requirements

- **C++23** compiler: Clang 17+, GCC 14+, or MSVC 2022+
- **CMake** 3.28 or newer
- **vcpkg** — used in manifest mode, auto-resolves dependencies

### Optional

- **FTXUI** — enables the terminal dashboard (auto-resolved by vcpkg when `LUMEN_ENABLE_DASHBOARD=ON`)
- **pybind11** — enables Python bindings when `LUMEN_ENABLE_PYTHON=ON`

## Installation

```sh
git clone https://github.com/user/lumen.git
cd lumen
cmake --preset debug
cmake --build build/debug
ctest --preset debug
```

For a release build without tests:

```sh
cmake --preset release
cmake --build build/release
```

## Using Lumen in your project

Add Lumen as a subdirectory dependency:

```cmake
add_subdirectory(path/to/lumen)
target_link_libraries(your_target PRIVATE lumen::lumen)
```

Or install system-wide (the library target `lumen::lumen` includes all necessary include paths):

```sh
cmake --install build/release --prefix /usr/local
```

```cmake
find_package(lumen REQUIRED)
target_link_libraries(your_target PRIVATE lumen::lumen)
```

## Your first program

```cpp
#include "lumen/lumen.h"

int main() {
    // Get the global core. Sinks register here.
    auto& core = lumen::core();

    // Add a terminal sink that sees everything.
    core.add_sink(
        std::make_unique<lumen::TerminalSink>(),
        lumen::always()
    );

    // Set process-wide tags attached to every record.
    core.set_process_tag("app", "my_app");
    core.set_process_tag("version", "1.0.0");

    // Emit a log.
    LOG_INFO("hello from Lumen");

    // Emit a metric.
    lumen::metric("items_processed", 150);

    // Track progress.
    auto bar = lumen::progress("loading", 100);
    for (int i = 0; i < 100; i++) {
        do_work(i);
        bar.tick();
    }

    core.flush();
}
```

Compile with:

```sh
cmake --preset debug && cmake --build build/debug
./build/debug/examples/basic
```

## Compile-time level control

Control which log levels survive compilation with `LUMEN_MIN_LEVEL`:

```sh
# Strip all DEBUG and TRACE calls from the binary
cmake --preset release -DLUMEN_MIN_LEVEL=INFO
```

Below the threshold, macro calls expand to `((void)0)` — no formatting, no allocation, no runtime cost. Metrics and progress are never elided.
