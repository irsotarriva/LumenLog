# Lumen — Agent Instructions

## Project

- **Repo:** ILog / **Project:** Lumen — structured C++20 telemetry library
- Zero-instrumentation, lock-free MPSC broker, 3 record types (`LogRecord`, `MetricRecord`, `ProgressRecord`)
- **Conventional commits** (`feat:`, `fix:`, `chore:`, `docs:`, `test:`, `refactor:`)

## Build & Test

```sh
cmake --preset debug
cmake --build build/debug
ctest --preset debug
# Single test:
ctest --test-dir build/debug -R TestName --output-on-failure
```

## Style (enforced by .clang-format + .clang-tidy)

| Entity | Convention |
|---|---|
| Classes / Concepts | `PascalCase` |
| Functions / methods | `snake_case` |
| Private members | `__snake_case` prefix |
| Protected members | `_snake_case` prefix |
| File names | `snake_case` |
| Macros / `#define` | `ALL_CAPS` |
| Header guards | `#ifndef` (not `#pragma once`) |
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
- **Emit path:** `LOG_*` macro → `RecordBuilder` (copies strings) → context merge + owned string block → CAS push to MPSC ring → dispatch thread evaluates predicates → sink queues. `Core::flush()` drains on the caller's thread and flushes sinks.
- **Context merge priority (first wins):** explicit `.tag()` > instance (`lumen::Tagged`) > scope (`LUMEN_SCOPE`) > thread (`set_thread_tag`) > process (`set_process_tag`).
- **Records own their strings** — builders copy message/tags into a stack scratch buffer, then pack them into one immutable `shared_ptr<const char[]>` block per record (one allocation, no locks). Never store a `string_view` into producer memory in a record.
- **Sink `on_*` methods MUST be non-blocking** — called on the single dispatch thread. Offload I/O to sink-owned threads.
- **Predicates** — compiled decision trees, immutable after registration. Composed with `&&`, `||`, `!`.
- **Bridges** — opt-in headers (`lumen/bridges/*.h`), no core changes needed. Ships separately for torch, root, eigen, openmp.
- **Python bindings** — optional pybind11, gated by `LUMEN_ENABLE_PYTHON=ON`. Same broker as C++.

## Key CMake Flags

| Flag | Default | Description |
|---|---|---|
| `LUMEN_MIN_LEVEL` | `DEBUG` | Elides logs below this level at compile time |
| `LUMEN_ENABLE_DASHBOARD` | `ON` | FTXUI terminal dashboard |
| `LUMEN_ENABLE_LATEX` | `OFF` | MicroTeX math rendering in sinks |
| `LUMEN_ENABLE_PYTHON` | `OFF` | pybind11 Python bindings |
| `LUMEN_ENABLE_REFLECTION` | `AUTO` | C++26 class-name harvesting |

## Implementation Plan

See **`MILESTONES.md`** for the phased implementation plan (5 milestones, branch-per-milestone strategy). Start at M1 if you're beginning fresh; check off completed items as you go.

## Testing

- **GTest.** One test file per source file under `tests/`. Test names describe the scenario.
- Write tests before or alongside implementation. Test the **public interface only**.
- Prefer `EXPECT_*` over `ASSERT_*` for non-fatal checks.
- Error-path tests are mandatory.
- Adding a third-party library requires approval — stop and ask.
