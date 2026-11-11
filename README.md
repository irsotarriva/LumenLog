# Lumen

Lumen is a C++23 library that records log messages, metrics and progress bars
as structured records carrying key/value tags. A logging statement pushes its
record onto a lock-free queue; one background thread delivers it to sinks
(terminal, files, a terminal dashboard, or your own), and each sink receives
only the records that its predicate on level and tags accepts.

Tags come from the statement itself and from context set elsewhere: the
process, the thread, the enclosing scopes and the object whose member function
is running. The statement does not repeat them.

Version 0.1.0. The API may still change between minor versions.

## Quick start

<!-- check: from=examples/quick_start.cpp -->
```cpp
// Quick start: a terminal sink, process tags, log statements, a metric and a
// progress bar. Every line goes to standard error.
#include <cstdint>
#include <iostream>
#include <utility>

#include "lumen/lumen.hpp"

int main() {
    lumen::TerminalSink::Config config;
    // Tags are shown only when listed here, in this order.
    config.inline_tags = {"app", "detector", "run"};
    auto terminal = lumen::TerminalSink::create(std::move(config));
    if (!terminal) {
        std::cerr << "cannot create the terminal sink: " << terminal.error().message() << '\n';
        return 1;
    }
    if (const auto id = lumen::core().add_sink(std::move(*terminal)); !id) {
        std::cerr << "cannot register the terminal sink: " << id.error().message() << '\n';
        return 1;
    }

    // Every record from now on carries app=quick_start and run=42.
    for (const auto& [key, value] : {std::pair{"app", "quick_start"}, std::pair{"run", "42"}}) {
        if (const auto set = lumen::set_process_tag(key, value); !set) {
            std::cerr << "cannot set the process tag " << key << ": " << set.error().message()
                      << '\n';
            return 1;
        }
    }

    LUMEN_LOG_INFO("starting");
    const double energy = 41.53;
    LUMEN_LOG_WARN("energy {:.1f} GeV above threshold", energy).tag("detector", "calorimeter");
    LUMEN_LOG_DEBUG("{} channels masked", 3).tag("detector", "tracker");
    LUMEN_LOG_ERROR("explicit tags win").tag("run", 43);

    lumen::metric("loss", 0.25).tag("epoch", 3);

    auto bar = lumen::progress("events", 3);
    for (uint32_t event = 0; event < 3; ++event) {
        bar.tick();
    }
    bar.finish();  // 3/3 was already sent by the last tick(), so nothing more is sent

    // Delivers everything logged so far and waits until the sink has written it.
    lumen::core().flush();
    return 0;
}
```

It prints, on standard error (`<time>` stands for the local time, `HH:MM:SS.mmm`):

<!-- check: output=examples/expected/quick_start.stderr -->
```text
<time> INFO  starting app=quick_start run=42
<time> WARN  energy 41.5 GeV above threshold app=quick_start detector=calorimeter run=42
<time> DEBUG 3 channels masked app=quick_start detector=tracker run=42
<time> ERROR explicit tags win app=quick_start run=43
<time> METRIC loss=0.25 app=quick_start run=42
<time> PROGRESS events 1/3 app=quick_start run=42
<time> PROGRESS events 2/3 app=quick_start run=42
<time> PROGRESS events 3/3 app=quick_start run=42
```

The `epoch` tag is on the metric record but not on the line: a terminal sink
shows only the tags named in `inline_tags`. Log, metric and progress lines can
come out in a different order from run to run; within one type the order is
the order of the statements. The level name is coloured when standard error is
a terminal (the exact rule is in [Sinks](docs/sinks.md#terminalsink)).

This program is `examples/quick_start.cpp`. ctest builds it, runs it and
compares its output with the text above (test `Example.QuickStart`). The other
examples are:

| Example | Shows |
|---|---|
| `examples/context_layers.cpp` | `lumen::Tagged` and `LUMEN_MEMBER_SCOPE`, `LUMEN_SCOPE`, thread tags, `LUMEN_LOOP`, and which tags each record carries |
| `examples/routing.cpp` | several sinks with predicates built in C++ and parsed from text; text, logfmt and JSON files; error handling; `Core::statistics()` |
| `examples/custom_sink.cpp` | a sink of your own with a writer thread, honouring the non-blocking contract |
| `examples/python/quick_start.py` | the Python module and the `logging` handler |

## Requirements

- A C++23 compiler and standard library with `<expected>`, `<format>` and
  `<source_location>`. Clang with GNU libstdc++ 13 does not work: Clang 18
  does not see `std::expected` in that library.
- Windows: experimental. MSVC is built and tested by one CI job that is
  allowed to fail, with the dashboard off; the dashboard has never been
  built on Windows.

The CI workflow (`.github/workflows/ci.yml`) has these jobs. Every one of
them fails the workflow when it fails, except
`windows-msvc-debug (experimental)`:

| Job | Toolchain | What it builds and runs |
|---|---|---|
| `linux-gcc-debug-asan` | GCC 13, libstdc++, Ubuntu 24.04, vcpkg | Debug, ASan + UBSan, dashboard; the Eigen and OpenMP bridge tests and the locale tests; fails if any test is skipped |
| `linux-gcc-tsan` | GCC 13, vcpkg | Debug, ThreadSanitizer, dashboard |
| `linux-gcc-release` | GCC 13, vcpkg | Release, dashboard; also the `build-settings` test preset |
| `linux-gcc-release-no-dashboard` | GCC 13, apt packages, no vcpkg | Release without the dashboard |
| `linux-clang-libcxx-debug-asan` | Clang 18, libc++ 18, vcpkg | Debug, ASan + UBSan, dashboard |
| `macos-appleclang-debug-asan` | Apple Clang, `macos-latest`, vcpkg | Debug, ASan + UBSan, dashboard |
| `python-bindings` | GCC 13, pybind11 from pip | Debug, ASan + UBSan, the Python module and its tests |
| `lint (clang-format, clang-tidy)` | clang-format 18, clang-tidy 18 | the `format-check` and `lint` targets, dashboard and Python module included |
| `coverage (gcovr)` | GCC 13, vcpkg | line and branch coverage of `src/` and `include/`, dashboard included; uploads the report and fails under its thresholds |
| `bridges (torch)` | GCC 13, libtorch 2.5.1 (CPU), Eigen and OpenMP | the bridge tests and the bridge documentation blocks |
| `bridges (root)` | conda-forge GCC 13 and ROOT 6.32 | the same for ROOT |
| `windows-msvc-debug (experimental)` | MSVC, `windows-latest`, vcpkg | Debug, no sanitizer, dashboard off; allowed to fail |
- CMake 3.28 or later.
- For the tests: GoogleTest and a Python 3 interpreter (the example and
  documentation checks run in Python; no Python package is needed).
- Optional: FTXUI for the terminal dashboard (`LUMEN_ENABLE_DASHBOARD`,
  on by default when FTXUI is found); pybind11 3.0 or later and Python 3.11
  or later for the Python module (`LUMEN_ENABLE_PYTHON`).

## Building

```sh
git clone https://github.com/irsotarriva/LumenLog.git
cd LumenLog
cmake --preset debug          # Debug, ASan + UBSan, tests and examples
cmake --build --preset debug
ctest --preset debug
```

The dashboard is built when FTXUI is found (from vcpkg or installed) and
left out otherwise; the configure output says which. `-DLUMEN_ENABLE_DASHBOARD=ON`
makes FTXUI required, `-DLUMEN_ENABLE_DASHBOARD=OFF` skips looking for it.

The configure presets are `debug`, `tsan` (ThreadSanitizer), `release`, `ci`
(Debug, ASan + UBSan, warnings as errors) and `coverage` (Debug with GCC's
`--coverage`, for gcovr). Each one builds in `build/<preset>`. The test
presets of the same names leave out the tests labelled `build-settings`,
which build and install Lumen with non-default settings and take minutes;
`ctest --preset build-settings` runs them against the `release` build.

With vcpkg: set the environment variable `VCPKG_ROOT` to a vcpkg checkout. The
presets then use its toolchain file, and vcpkg installs the manifest features
of `vcpkg.json` that the options select: `tests` (GoogleTest, with
`LUMEN_BUILD_TESTS`), `dashboard` (FTXUI, with `LUMEN_ENABLE_DASHBOARD`) and
`python` (pybind11, with `LUMEN_ENABLE_PYTHON`). `vcpkg.json` lists Lumen's
own dependencies; this repository is not a vcpkg port of Lumen.

Without vcpkg: leave `VCPKG_ROOT` unset; CMake then finds installed packages
(set `CMAKE_PREFIX_PATH` if they are not in a default location):

```sh
cmake --preset release -DLUMEN_ENABLE_DASHBOARD=OFF
cmake --build --preset release
```

The CI job `linux-gcc-release-no-dashboard` builds this way.

Sanitizers and multi-configuration generators (Ninja Multi-Config, Visual
Studio, Xcode): `LUMEN_SANITIZER` instruments the Debug configuration only,
and defaults to `address` for a top-level build; Release, RelWithDebInfo and
MinSizeRel are never instrumented. With a single-configuration generator
(the presets use one) it instruments the one build, whatever its
`CMAKE_BUILD_TYPE`.

## Using Lumen from CMake

As a subdirectory of your project (tests, examples, install rules and
sanitizers are then off by default, and the dashboard is built only if FTXUI
is found):

```cmake
add_subdirectory(LumenLog)
target_link_libraries(my_program PRIVATE lumen::lumen)
```

As an installed package:

```sh
cmake --install build/release --prefix /opt/lumen
```

```cmake
find_package(lumen 0.1 CONFIG REQUIRED)   # with CMAKE_PREFIX_PATH=/opt/lumen
target_link_libraries(my_program PRIVATE lumen::lumen)
```

Both ways are tested by `Consumer.FindPackageAndAddSubdirectory`, which
builds `tests/consumer/` against an install and against the source tree.
`lumen::lumen` carries the include directories and the C++23 requirement.
The build settings (`LUMEN_MIN_LEVEL`, the queue capacities) are written into
the generated header `lumen/config.hpp`, which is installed with the others,
so a build that does not use CMake (`-I<prefix>/include` and
`<prefix>/lib/liblumen.a`) sees the values the library was built with, and
one that defines a different queue capacity fails to compile.
`Consumer.WarnLevelAndSmallQueues` and `Consumer.FatalLevel` (label
`build-settings`) check this with non-default settings.

## Build options

<!-- check: options-table -->
| Option | Default | Meaning |
|---|---|---|
| `LUMEN_MIN_LEVEL` | `DEBUG` | Lowest log level compiled in: `TRACE`, `DEBUG`, `INFO`, `WARN`, `ERROR` or `FATAL`. Statements below it are removed at compile time. |
| `LUMEN_LOG_CAPACITY` | `65536` | Log records the log queue holds (a power of two) |
| `LUMEN_METRIC_CAPACITY` | `16384` | Metric records the metric queue holds (a power of two) |
| `LUMEN_PROGRESS_CAPACITY` | `4096` | Progress records the progress queue holds (a power of two) |
| `LUMEN_ENABLE_DASHBOARD` | `ON` if FTXUI is found, else `OFF` | Build `DashboardSink`. `ON` requires FTXUI |
| `LUMEN_ENABLE_PYTHON` | `OFF` | Build the Python module `lumen_bindings` (needs pybind11 3.0+) |
| `LUMEN_SANITIZER` | `address` for a top-level build with a Debug configuration, else `none` | `none`, `address` (ASan + UBSan) or `thread` (TSan); with a multi-configuration generator, for the Debug configuration only |
| `LUMEN_BUILD_TESTS` | `ON` when top level | Build the tests (needs GoogleTest and Python 3) |
| `LUMEN_BUILD_EXAMPLES` | `ON` when top level | Build the examples |
| `LUMEN_INSTALL` | `ON` when top level | Install rules and the `lumen` CMake package |
| `LUMEN_INSTALL_CMAKEDIR` | `${CMAKE_INSTALL_LIBDIR}/cmake/lumen` | Where the package files are installed |
| `LUMEN_WARNINGS_AS_ERRORS` | `ON` when top level | `-Werror` or `/WX` |
| `LUMEN_REQUIRED_BRIDGE_FRAMEWORKS` | empty | Bridge frameworks whose tests must be built (`Eigen3;OpenMP;Torch;ROOT`); configuring fails if one is missing |

`LUMEN_MIN_LEVEL` and the capacities change what the public headers compile
to. They are written into the generated header `lumen/config.hpp` (installed
with the library), not passed as definitions, so every user of a build of
Lumen sees the same values, with CMake or without.

## What is in the library

- **Records** (`lumen/record.hpp`): `LogRecord`, `MetricRecord`,
  `ProgressRecord`. Each carries at most 16 tags, a nanosecond timestamp and
  a thread number, and owns its strings. [Usage](docs/usage.md)
- **Producers**: `LUMEN_LOG_TRACE` … `LUMEN_LOG_FATAL` with `std::format`
  arguments checked at compile time, `lumen::metric`, `lumen::progress`,
  `LUMEN_LOOP`. [Usage](docs/usage.md)
- **Context**: process tags, thread tags, `LUMEN_SCOPE`, `lumen::Tagged`
  objects. [Usage](docs/usage.md#context)
- **Predicates**: built in C++ (`level_at_least`, `tag_equals`, `&&`, …) or
  parsed from text (`level >= WARN && run == 42`). [Predicates](docs/predicates.md)
- **Sinks**: `TerminalSink`, `FileSink` (text, logfmt or JSON lines, with
  rotation), `DashboardSink` (full-screen terminal view), `NullSink`, and your
  own. [Sinks](docs/sinks.md)
- **Python**: the same Core from Python, and a `logging.Handler`.
  [Python](docs/python.md)
- **Bridges**: helpers for Eigen, OpenMP, ROOT and libtorch, below.

### Bridges

Optional bridge headers turn values held by Eigen, OpenMP, ROOT and libtorch
objects into metric records: matrix norms and solver convergence, OpenMP work
balance, TTree sizes, training loss, gradient norm and learning rate. They
hook nothing; your code calls them where it has the values. Each is
header-only and empty unless its framework is available.

<!-- check: requires=torch prelude=bridge_torch wrap=body -->
```cpp
#include "lumen/bridges/torch.hpp"

const lumen::bridges::TorchTrainingMetrics metrics("regressor");
LUMEN_LOOP("epoch", epoch, n_epochs) {
    optimizer.zero_grad();
    const torch::Tensor loss = torch::mse_loss(model->forward(inputs), targets);
    loss.backward();
    static_cast<void>(metrics.emit_loss(loss));             // torch.loss       model=regressor epoch=<e>
    static_cast<void>(metrics.emit_gradient_norm(*model));  // torch.grad_norm  (global L2 norm)
    optimizer.step();
}
```

The `static_cast<void>` drops the `std::expected` results for brevity;
[Bridges](docs/bridges.md) handles them, and lists every metric, tag and
error and the CI status of each bridge.

## Limits and guarantees

The numbers below are the constants of `lumen/limits.hpp` and the defaults of
the sinks' `Config`, except the builder's buffer
(`lumen/detail/string_scratch.hpp`).

- **Tags.** A record holds at most 16 tags (`max_record_tags`): the explicit
  `.tag()` calls first, then context, highest layer first. A tag that does not
  fit is left out and counted in `Core::statistics().tags_dropped`. The
  process layer holds 16 keys, a thread 8, a `Tagged` object 8; setting a new
  key in a full table fails with `LumenError::TagTableFull` and changes
  nothing.
- **Queues.** Each record type has a bounded queue: 65536 log records, 16384
  metrics, 4096 progress records by default (`LUMEN_LOG_CAPACITY` etc.). A
  producer never waits: a record submitted to a full queue is refused and
  counted in `statistics().logs.dropped_queue_full` (and `.metrics`,
  `.progress`), and the first refusal is reported once through the Core's
  diagnostic handler, which writes to standard error unless you replace it
  (`Core::set_diagnostic_handler`).
- **Sink queues.** The shipped sinks write from a thread of their own, through
  a queue of `max_queued_lines` lines (10000 for `TerminalSink`, 65536 for
  `FileSink`). When it is full the line is dropped and counted in the sink's
  `statistics().lines_dropped`; the first drop is reported once.
- **Threads.** Any thread may log. A producer takes no lock, except to refresh
  its copy of the process tags after they changed, and makes a system call
  only when the dispatch thread is asleep. Submission allocates one block on
  the heap for the record's strings; a statement whose strings exceed the
  builder's 512-byte stack buffer allocates more. Sink callbacks run on one
  thread at a time, never concurrently with each other; they must not block
  (see [Sinks](docs/sinks.md#the-contract)).
- **Order.** Records of one type reach a sink in the order they were queued.
  Records of different types are not ordered with each other.
- **Flush.** `Core::flush()` delivers every record submitted before the call,
  on the calling thread, then calls `flush()` on every sink, and returns when
  that is done. For the shipped sinks that means written and the stream
  flushed (no `fsync`).
- **Shutdown and late logging.** `lumen::core()` is created on first use and
  shuts down during static destruction: it stops its thread, delivers what is
  queued, flushes and destroys its sinks. A record submitted after that, for
  example from the destructor of a static object, is not delivered; it is
  counted in `statistics().logs.dropped_after_shutdown` (and the same for the
  other types). A record submitted by another thread while shutdown runs may
  be lost without being counted: stop your threads first.
- **`LUMEN_LOG_FATAL`** delivers every record queued before it, then the fatal
  record, flushes every sink and calls `std::terminate()`, which aborts the
  process unless you installed another terminate handler. Destructors and
  `atexit` functions do not run.
- **Compile-time level.** Statements below `LUMEN_MIN_LEVEL` (default
  `DEBUG`, so `LUMEN_LOG_TRACE` is removed by default) are discarded at
  compile time: they produce no code and their arguments are not evaluated.
  Fatal statements are never discarded. Metrics, progress and Python calls
  are not affected.
- **Errors.** Fallible calls return `std::expected<T, std::error_code>` (or
  `PredicateParseError` for `parse_predicate`); the codes are listed in
  `lumen/error.hpp`. An exception thrown by a sink callback is caught, counted
  (`Core::sink_exception_count`) and reported once per sink; delivery goes on.

## Documentation

| Page | Contents |
|---|---|
| [Getting started](docs/getting-started.md) | Building, consuming, the first program |
| [Usage](docs/usage.md) | Log statements, metrics, progress, context, compile-time level, fatal |
| [Predicates](docs/predicates.md) | Building and parsing filters, the rules they follow |
| [Sinks](docs/sinks.md) | The shipped sinks, their formats and queues, writing your own |
| [Python](docs/python.md) | The Python module and the `logging` handler |
| [Bridges](docs/bridges.md) | Helpers for Eigen, OpenMP, ROOT and libtorch |
| [Headers](docs/api.md) | Which header declares what |
| [Design](docs/design.md) | How the library works inside, for maintainers |

Every C++ block in these pages is compiled by the build (a bridge block only
when its framework is found), and the complete programs among them are run by
ctest. Python blocks are run when the module is built. Quoted examples and
outputs must equal the files they quote. `Docs.Check` verifies this, and
`tests/docs/check_docs.py` states the rules.

## License

[MIT](LICENSE).
