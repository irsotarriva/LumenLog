# Lumen — Agent Instructions

## Project

- **Repo:** ILog / **Project:** Lumen — structured C++20 telemetry library
- Zero-instrumentation, lock-free MPSC broker, 3 record types (`LogRecord`, `MetricRecord`, `ProgressRecord`)
- **Conventional commits** (`feat:`, `fix:`, `chore:`, `docs:`, `test:`, `refactor:`)

## Build & Test

Presets: `debug` (ASan + UBSan), `tsan` (ThreadSanitizer), `release`, `ci` (Debug, ASan + UBSan, `-Werror`).
They use vcpkg when `VCPKG_ROOT` is set (`cmake/vcpkg_if_available.cmake`) and system packages otherwise.

```sh
cmake --preset debug                 # add -DLUMEN_ENABLE_DASHBOARD=OFF without FTXUI
cmake --build --preset debug
ctest --preset debug
# Single test:
ctest --test-dir build/debug -R TestName --output-on-failure
# Formatting and static analysis (clang-format / clang-tidy 18):
cmake --build --preset debug --target format-check   # or `format` to rewrite
cmake --build --preset debug --target lint
# Python bindings (pybind11 >= 3.0); adds the ctest test Python.Bindings:
cmake --preset debug -DLUMEN_ENABLE_PYTHON=ON -Dpybind11_DIR=$(python3 -m pybind11 --cmakedir)
```

The ctest test `Consumer.FindPackageAndAddSubdirectory` installs Lumen into a
temporary prefix and builds `tests/consumer/` against it with `find_package(lumen)`
and with `add_subdirectory`. `lumen_bench` is built but not run by ctest.

## Style (enforced by .clang-format + .clang-tidy)

| Entity | Convention |
|---|---|
| Classes / Concepts | `PascalCase` |
| Enums | `enum class`, `PascalCase` enumerators (`LogLevel::Info`) |
| Functions / methods | `snake_case` |
| Private members | `snake_case_` suffix (never a leading `__`: reserved) |
| Protected members | `_snake_case` prefix |
| File names | `snake_case` |
| Macros / `#define` | `ALL_CAPS` |
| Header guards | `#ifndef LUMEN_<PATH>_HPP` (not `#pragma once`) |
| Headers | `.hpp` for declaration + template; `.cpp` for implementation |

**No exceptions.** All fallible operations return `std::expected<T, std::error_code>`.
- `[[nodiscard]]` on every meaningful return value
- `<cstdint>` types only; no bare `int`/`long` where size matters
- No C-style casts; `explicit` on all single-arg constructors
- `const` on everything that should not change
- `constexpr`/`consteval` wherever the value is a compile-time constant
- Concepts over SFINAE; policy/mixin over inheritance
- No raw owning pointers (`std::unique_ptr` by default)
- `auto` only where type is unambiguous at a glance
- Platform `#ifdef` uses CMake-defined macros (`PLATFORM_WINDOWS`, `PLATFORM_LINUX`, `PLATFORM_MACOS`), never `_WIN32`/`__APPLE__` directly

## Architecture

- **`lumen::core()`** — Meyer's singleton. Owns 3 MPSC ring buffers + dispatch thread + sink registry.
- **Emit path:** `LUMEN_LOG_*` macro (`lumen/macros.hpp`; short `LOG_*` aliases unless `LUMEN_NO_SHORT_MACROS` is defined) → `RecordBuilder` (copies strings) → context merge + owned string block → CAS push to MPSC ring → dispatch thread evaluates predicates → sink queues. `Core::flush()` drains on the caller's thread and flushes sinks.
- **Context merge priority (first wins):** explicit `.tag()` > instance (`lumen::Tagged`) > scope (`LUMEN_SCOPE`) > thread (`set_thread_tag`) > process (`set_process_tag`).
- **Records own their strings** — builders copy message/tags into a stack scratch buffer, then pack them into one immutable `shared_ptr<const char[]>` block per record (one allocation, no locks). Never store a `string_view` into producer memory in a record.
- **Sink `on_*` methods MUST be non-blocking** — called on the single dispatch thread. Offload I/O to sink-owned threads.
- **Predicates** — compiled decision trees, immutable after registration. Composed with `&&`, `||`, `!`.
- **Bridges** — opt-in headers (`bridges/*.hpp`), no core changes needed. Ships separately for torch, root, eigen, openmp.
- **Python bindings** — optional pybind11, gated by `LUMEN_ENABLE_PYTHON=ON`. Same broker as C++.

## Key CMake Flags

| Flag | Default | Description |
|---|---|---|
| `LUMEN_MIN_LEVEL` | `DEBUG` | Elides logs below this level at compile time. CMake validates the name and, when it is not `DEBUG`, passes its value as `LUMEN_MIN_LEVEL_VALUE` (0 = Trace … 5 = Fatal); the header defaults to Debug |
| `LUMEN_LOG_CAPACITY`, `LUMEN_METRIC_CAPACITY`, `LUMEN_PROGRESS_CAPACITY` | `65536`, `16384`, `4096` | Ring buffer slots (powers of two); passed to consumers only when not the default |
| `LUMEN_SANITIZER` | `address` for a top-level Debug build, else `none` | `none`, `address` (ASan + UBSan) or `thread` (TSan) |
| `LUMEN_ENABLE_DASHBOARD` | `ON` (`OFF` on Windows) | FTXUI terminal dashboard |
| `LUMEN_ENABLE_PYTHON` | `OFF` | pybind11 Python module (the core library does not depend on pybind11) |
| `LUMEN_BUILD_TESTS`, `LUMEN_BUILD_EXAMPLES` | `ON` when top level | Tests (needs GTest) and examples |
| `LUMEN_INSTALL` | `ON` when top level | Install rules and the `lumen` CMake package (`lumen::lumen`) |
| `LUMEN_WARNINGS_AS_ERRORS` | `ON` when top level | `-Werror` / `/WX` |

vcpkg manifest features: `tests` (gtest), `dashboard` (ftxui), `python` (pybind11), selected from these flags.

## Implementation Plan

`MILESTONES.md` is a local planning scaffold for agents. It is git-ignored and is not part of the repository; do not commit it. Audit reports live in the git-ignored `audits/` directory.

## Testing

- **GTest.** One test file per source file under `tests/`. Test names describe the scenario.
- Write tests before or alongside implementation. Test the **public interface only**.
- Prefer `EXPECT_*` over `ASSERT_*` for non-fatal checks.
- Error-path tests are mandatory.
- Adding a third-party library requires approval — stop and ask.
