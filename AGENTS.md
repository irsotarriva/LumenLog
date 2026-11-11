# Lumen — instructions for agents

## Project

- Repository `LumenLog`, project and CMake package `lumen`, version 0.1.0.
- A C++23 structured-telemetry library: log, metric and progress records with
  tags, a lock-free queue per record type, one dispatch thread, sinks chosen
  by predicates. Optional pybind11 module (`python/`) and header-only bridges
  (`include/lumen/bridges/`).
- The public headers are the specification. Each states what its
  declarations do, on which thread, and with which errors. A change of
  behaviour changes the header comment in the same commit.
- Conventional commits: `feat:`, `fix:`, `refactor:`, `test:`, `docs:`,
  `build:`, `ci:`, `chore:`.

## Layout

`docs/design.md` has the full table of which file owns what. In short:

- `include/lumen/` public headers, one responsibility each (`docs/api.md`
  lists them); `include/lumen/detail/` internal headers: those the public
  headers include, and `ring_buffer.hpp`, which only `src/` includes.
- `src/` the implementation: `core.cpp` (queues, dispatcher, registry,
  shutdown), `context.cpp` (all context state), `predicate*.cpp`, the sinks
  and their shared pieces (`record_format`, `line_writer`, `text_encoding`,
  `time_format`, `rotating_file`, `terminal_output`), the dashboard
  (`dashboard_*`, FTXUI only in `dashboard_sink.cpp` and `dashboard_frame.*`).
- `tests/` GTest files, `test_<name>.cpp` for `src/<name>.cpp`; the parts
  of the Core in their own files (`delivery`, `sink_registry`, `wakeup`,
  ...) are tested through `Core` in `test_core.cpp`, the ring buffer in
  `test_transport.cpp`, the scope guards in `test_scope.cpp`, and
  `dashboard_frame.cpp` through `test_dashboard_sink.cpp`; a few programs
  (`late_logging.cpp`, `blocked_stderr.cpp`, ...) run as their own process;
  `tests/compile_checks/` header
  and macro checks; `tests/consumer/` the install and `add_subdirectory`
  test; `tests/bridges/` bridge tests against the real frameworks;
  `tests/python/` the module's tests; `tests/docs/` the documentation check.
- `examples/` programs run by ctest against `examples/expected/`.
- `cmake/` build settings, sanitizers, install, lint targets, the vcpkg hook.

## Build and test

Presets (`CMakePresets.json`): `debug` (ASan + UBSan), `tsan`, `release`,
`ci` (Debug, ASan + UBSan, warnings as errors), `coverage` (GCC
`--coverage`, for gcovr). They load vcpkg when `VCPKG_ROOT` is set and use
installed packages otherwise.

```sh
cmake --preset debug          # the dashboard is built only where FTXUI is found
cmake --build --preset debug
ctest --preset debug          # every test except the label build-settings
ctest --test-dir build/debug -R TestName --output-on-failure
cmake --build --preset debug --target format-check  # or format, to rewrite
cmake --build --preset debug --target lint          # clang-tidy 18
ctest --preset build-settings # after building the release preset; slow
cmake --preset debug -DLUMEN_ENABLE_DASHBOARD=OFF -DLUMEN_ENABLE_PYTHON=ON \
      -Dpybind11_DIR=$(python3 -m pybind11 --cmakedir)   # adds Python.Bindings, Example.Python, Docs.Python.*
```

The tests need GoogleTest and a Python 3 interpreter. `lumen_bench_transport`
is built but not run by ctest. `Consumer.FindPackageAndAddSubdirectory`
builds Lumen twice and is the slowest test of the default presets; the tests
labelled `build-settings` (`Consumer.WarnLevelAndSmallQueues`,
`Consumer.FatalLevel`) build Lumen with non-default settings and run only
with the `build-settings` test preset. `lint` runs clang-tidy over the
translation units of the build in `src/`, `examples/`, `tests/` and
`python/` (headers through the files that include them): the dashboard and
`python/` only when the build compiles them, and never the two OpenMP
bridge files compiled with GCC's `-fopenmp` (`cmake/lint.cmake` says why).

## Style

The owner's rules; `.clang-format` and `.clang-tidy` enforce part of them.

- Never throw. Fallible operations return `std::expected<T, std::error_code>`.
  Catch exceptions from external code at the boundary and convert them.
- Never swallow an error silently. If ignoring one is intended, say why in a
  comment.
- `[[nodiscard]]` on every function whose result matters.
- `<cstdint>` types; no bare `int`, `long` or `unsigned` where size matters.
- No C-style casts. `explicit` on single-argument constructors.
- `const` on everything that does not change; `constexpr` where possible.
- `auto` only where the type is obvious at a glance.
- No raw owning pointers; a raw non-owning pointer carries a
  `// non-owning` comment.
- No raw arrays (`std::array`), no `printf` family (`std::format`, streams),
  no `NULL`.
- Macros only for include guards, platform and build flags, and the logging
  front end that needs `std::source_location` or statement semantics.
  Constants are `inline constexpr`.
- No mutable globals unless unavoidable; a `thread_local` for ambient context
  has a single owner and a comment saying so.
- Concepts on every template parameter; no SFINAE.
- Scoped enums with `PascalCase` enumerators (`LogLevel::Info`).
- Naming: classes and concepts `PascalCase`; functions, methods, variables
  `snake_case`; private members `snake_case_` (never a leading `__`);
  protected members `_snake_case`; files `snake_case`; macros `ALL_CAPS`.
  The trailing underscore is for private members only.
- Headers: `.hpp` with `#ifndef LUMEN_<PATH>_HPP` guards (not
  `#pragma once`); declarations in `.hpp`, implementation in `.cpp`,
  templates in the header. A header lets a reader use the API without
  opening the `.cpp`.
- Platform `#ifdef` only with `PLATFORM_WINDOWS`, `PLATFORM_LINUX`,
  `PLATFORM_MACOS` (defined by CMake for the library's own sources).
- `std::filesystem::path` for paths.
- Concurrency: `std::scoped_lock` or `std::unique_lock`; every shared
  variable has a comment naming the mutex that guards it. `std::thread`, not
  `std::jthread`, because Apple's libc++ lacks it; say so in a comment.
- Policy, mixin or `std::variant` over inheritance; virtual dispatch only for
  open sets (sinks).
- Short single-purpose functions, cyclomatic complexity under about 10. One
  responsibility per file.
- Comments explain why, never what. They never mention a conversation, an
  agent, an audit, a request or the history of the change. No banner
  comments. Example names must make sense to a stranger.
- No new third-party dependency without the owner's approval: stop and ask.

## Testing

- GTest. One test file per source file. Test the public interface.
- A test's name states exactly what its assertions check.
- Mutation rule: for every test you add or rewrite, break the behaviour once
  (a temporary edit you revert) and confirm the test fails.
- No test whose only assertion is `SUCCEED()` or `EXPECT_NO_THROW`.
- No sleeps to wait for delivery: `Core::flush()` is synchronous.
- Error paths are mandatory.
- Tests must not leak state into each other. Use `tests/test_support.hpp`:
  `LocalCoreTest` (a Core of the test's own, diagnostics captured),
  `GlobalCoreTest` (a capture sink on `lumen::core()`, removed afterwards),
  `CleanContext` (clears process and thread tags before and after).
- `EXPECT_*` for non-fatal checks; `ASSERT_*` before indexing a container.

## Documentation

- Every C++ block in `README.md`, `AGENTS.md` and `docs/*.md` is compiled by
  the build (a bridge block only where its framework is found) and a
  complete program is also run by ctest; every Python block is run by ctest
  when the module is built. `tests/docs/check_docs.py` states the
  directives (`from=`, `prelude=`, `wrap=body`, `requires=`, `output=`,
  `lines-in=`, `illustrative=`). A block that is not classified fails
  `Docs.Check`.
- `Docs.Check` also checks the names a document uses, as its docstring
  states: a `lumen::` name or `Class::member` must be declared in the
  headers, a header, path, preset or target must exist, a macro or CMake
  variable must be a word of the source tree, a `lumen.` name one the module
  registers. It also fails when the README options table or the
  `docs/api.md` header table is incomplete. It does not check what prose
  says about behaviour.
- A change to an example's output changes `examples/expected/` and the
  documents that quote it, in the same commit.

## Facts not to get wrong

- The dashboard (`LUMEN_ENABLE_DASHBOARD`) compiles only where FTXUI is
  installed. Left unset, the option is on where FTXUI is found and off
  otherwise (configuring says which); `-DLUMEN_ENABLE_DASHBOARD=ON` without
  FTXUI fails. The CI jobs that use vcpkg, except the Windows one, are where
  the dashboard is compiled, tested and linted.
- `lumen/config.hpp` is generated (from `cmake/config.hpp.in`) into the build
  directory and installed; it carries `LUMEN_MIN_LEVEL` and the queue
  capacities. Do not add them as compile definitions.
- Clang with GNU libstdc++ 13 cannot build Lumen (`std::expected` is hidden).
  Use GCC 13 or later, or Clang with libc++.
- Windows is experimental: the MSVC CI job,
  `windows-msvc-debug (experimental)`, runs with the dashboard off and is the
  only job allowed to fail. Keep code portable (no POSIX-only headers outside
  `PLATFORM_*` blocks).
- Never rename compiler or preprocessor built-ins (`__has_include`,
  `__COUNTER__`, `__VA_ARGS__`, `_OPENMP`, `__cpp_*`) in a mass rename.
- `MILESTONES.md` and `audits/` are local, git-ignored planning files. Never
  commit them.
