---
name: cpp-style
description: >
  C++ coding standards for Claude Code. Use this skill whenever writing, editing,
  reviewing, or generating any C++ code — including new files, refactors, bug fixes,
  adding features, or creating CMakeLists.txt. If C++ is involved in any way, read
  this skill first.
---

# C++ Coding Standards

Read fully before writing any code. When a decision is not covered here, ask.

---

## 0. Cardinal Rules

- **Never add a third-party library without explicit approval.** Stop and ask, explaining
  the benefit, exception-handling burden, and supply-chain implications. Wait for yes.
- **Never throw.** The codebase never produces exceptions. All fallible operations return
  `std::expected<T, std::error_code>`. Every exception from an external library is caught
  at the boundary and converted.
- **Ask rather than assume** on anything ambiguous.

---

## 1. Core Principles

These principles govern every decision. When in doubt, apply them directly.

- **Be pedantic.** State intent explicitly. If something is constant, mark it. If a
  return value must not be discarded, say so. Prefer the version of the code that leaves
  less to the reader's assumption.
- **Be explicit.** No implicit conversions. No `auto` where the type is not obvious at a
  glance. No unenforced contracts. If the compiler can check it, make it check it.
- **Be defensive.** Assume all inputs are malformed until validated. Assert internal
  invariants. Return errors for anything you do not control.
- **Use modern C++.** No C idioms. Prefer standard library types and algorithms over
  hand-rolled equivalents. Prefer compile-time over runtime where readability permits.
- **Prefer performance, but not at the cost of clarity** without a comment explaining
  the trade-off and its measured justification.
- **RAII everywhere.** Resources are owned by objects whose destructors release them.
  No manual resource management.

---

## 2. Language Standard and Build System

- Target **C++23** unless the project CMake specifies otherwise.
- **CMake** + **vcpkg** (manifest mode, `vcpkg.json`). All new dependencies require
  approval before being added.
- Prefer **C++20 modules** over headers where the toolchain and dependencies support it.
  Fall back to headers when a dependency forces it; note the reason in a comment.
- Headers: use **header guards** (not `#pragma once`). Separate `.hpp` (declaration)
  from `.cpp` (implementation). Templates live in `.hpp`.
- Enable warnings as errors: `-Wall -Wextra -Wpedantic -Werror`. Sanitizers (ASan,
  UBSan) in Debug builds.

---

## 3. Naming Conventions

| Entity | Convention | Example |
|---|---|---|
| Classes / Structs / Concepts | `PascalCase` | `ParticleTracker`, `Serializable` |
| Functions / methods | `snake_case` | `compute_energy()` |
| Public member variables | `snake_case` | `hit_energy` |
| Protected member variables | `snake_case` with `_` prefix | `_hit_energy` |
| Private member variables | `snake_case` with `__` prefix | `__hit_energy` |
| Local variables | `snake_case` | `total_count` |
| Type aliases / `using` | `PascalCase` | `using EnergyMap = ...` |
| Template parameters | `PascalCase` | `template<typename ValueType>` |
| Scoped enum values | `PascalCase` | `Color::DarkRed` |
| File names | `snake_case` | `particle_tracker.cpp` |
| `#define` constants / macros | `ALL_CAPS` | `PLATFORM_WINDOWS` |
| Concepts | `PascalCase` + adjective form | `Serializable`, `Iterable` |

Names must be self-documenting. A well-chosen name is the primary documentation for
a parameter or variable. Abbreviate only when the domain makes it universal
(e.g. `eta`, `phi` in HEP).

---

## 4. Headers as API Contracts

The `.hpp` must let a reader understand how to *use* a class or function without
opening the `.cpp`.

- Parameter names must be descriptive enough to document themselves. `timer_seconds`
  not `t`. `lower_bound` not `lo`.
- Add a comment on a declaration only when something is non-obvious that the name
  and type cannot express — unexpected units, a non-obvious precondition, a surprising
  constraint.
- Add a `// Rationale:` comment for non-obvious design decisions.
- Comments explain *why*, not *what*. Never restate what the code already says clearly.
- Add `// TODO:` for known gaps; never leave silent unfinished logic.
- Keep headers free of implementation detail. Use Pimpl or module partitions when
  hiding internals matters.

---

## 5. Type Safety and Explicitness

- **Integer sizes**: always use `<cstdint>` types (`int32_t`, `uint64_t`, etc.).
  Never use `int`, `long`, or `unsigned` where size matters.
- **No implicit conversions.** Use named casts: `static_cast`, `reinterpret_cast`
  (C API boundary only, with a comment), `const_cast` (last resort, with a comment),
  `std::bit_cast` for type-punning. Never C-style casts `(T)x`.
- **All single-argument constructors are `explicit`.**
- **`auto`** only where the type is unambiguous at the call site (range-for iterators,
  lambdas, `std::expected` chain results). Explicit types everywhere else.
- **`[[nodiscard]]`** on every function whose return value must not be silently dropped.
- **`[[likely]]` / `[[unlikely]]`** when one branch is strongly dominant; add a comment
  stating why.
- **`constexpr` / `consteval`** wherever the value is a compile-time constant.
- **`const`** on everything that should not change, unless there is a measurable
  performance reason not to — in which case add a `// Perf:` comment.

---

## 6. Error Handling

All fallible operations return `std::expected<T, std::error_code>`. The project never
throws.

- Catch all external library exceptions at the boundary; convert to `std::error_code`
  using a custom `std::error_category` for domain-specific errors.
- Chain with `.and_then()` / `.transform()` / `.or_else()`. Avoid nested
  `if (!result)` ladders.
- Never swallow an error silently. If ignoring is intentional, add a comment.

---

## 7. Defensive Programming

- `assert()` for internal invariants the caller must guarantee (debug-mode checks).
- Runtime validation + `std::unexpected` for anything from outside the codebase
  (files, network, user input). Never `assert` on external data.
- Always null-check before dereferencing unless the type system guarantees safety.
- Use `std::span<T>` over raw pointer + length.
- No non-trivial global variables. `#define` constants and `inline constexpr` are fine;
  mutable global state is not.

---

## 8. Modern C++ — No C Idioms

- No raw `new`/`delete`. Use `std::unique_ptr` (default) or `std::shared_ptr`
  (shared ownership only — document why). Raw pointers are for non-owning observation
  only; mark them with a `// non-owning` comment.
- No raw arrays. Use `std::array` (fixed) or `std::vector` (dynamic).
- No `NULL`. Use `nullptr`.
- No `printf`/`scanf`. Use `std::format` or streams.
- No `#define` for constants or logic. Macros are for include guards, platform
  conditional compilation, and `ALL_CAPS` named build flags only.
- Choose containers for their access pattern; document non-obvious choices.

---

## 9. Portability

Code must compile and run correctly on all target platforms without modification to
shared code.

- Never assume integer byte width — use `<cstdint>` types (§5).
- Never assume endianness. If byte-order conversion is needed, use `std::byteswap`
  (C++23) with a comment explaining why.
- Always use `std::filesystem::path` for file system paths. Never use raw string
  literals or OS-specific path separators.
- **Platform-specific implementations** belong in the same `.cpp` file, separated by
  `#ifdef` blocks using CMake-defined macros (e.g. `PLATFORM_WINDOWS`, `PLATFORM_LINUX`).
  Never use compiler-predefined macros (`_WIN32`, `__APPLE__`) directly — if the
  required CMake macro is not defined in `CMakeLists.txt`, check there first and ask
  if it is missing.
- The `.hpp` must present a single, platform-agnostic interface. Platform branching is
  an implementation detail invisible to callers.

---

## 10. Composition and Polymorphism

Prefer compile-time composition over inheritance. Use the mechanism that fits the problem:

- **Policy / mixin pattern**: when composing an object from orthogonal, stateless (or
  self-contained) behavioral axes known at compile time. Policies are template parameters;
  `using` aliases name concrete configurations.
- **Concepts** (`PascalCase` adjective form, e.g. `Serializable`): to constrain template
  parameters at API boundaries. Every unconstrained `typename T` is a bug.
- **`std::variant` + `std::visit`**: when the set of types is closed and known at compile
  time but dispatch must happen at runtime.
- **Abstract base class with virtual dispatch**: only when the set of types is open and
  genuinely unknown at compile time. Keep hierarchies shallow (max 2 levels). Virtual
  destructors are mandatory on any heap-allocated base.
- No SFINAE. Rewrite with Concepts.

---

## 11. Concurrency

- `std::jthread` over `std::thread`.
- `std::scoped_lock` or `std::unique_lock` — never raw `lock()`/`unlock()`.
- `std::atomic<T>` for simple shared flags or counters.
- Every shared variable has a comment naming the mutex that protects it.

---

## 12. Data Layout and Performance

- Prefer SoA (Structure of Arrays) over AoS for bulk-processed data to enable SIMD.
  Add a `// Layout: SoA for SIMD` comment.
- Avoid heap allocations in hot paths; prefer stack or pool allocators.
- Performance optimizations that hurt clarity require a `// Perf:` comment with the
  measured justification.

---

## 13. Testing (GTest, TDD)

- Write tests before implementation. Tests mirror the source tree under `tests/`.
- One test file per source file. Test names describe the scenario.
- Test the public interface only. Always include an error-path test.
- Prefer `EXPECT_*` over `ASSERT_*` for non-fatal checks.

---

## Quick Reference Checklist

- [ ] `<cstdint>` types used; no bare `int`/`long` where size matters
- [ ] No C-style casts; no implicit conversions
- [ ] All single-arg constructors `explicit`
- [ ] `auto` only where type is unambiguous
- [ ] All fallible functions return `std::expected`; no `throw`
- [ ] External exceptions caught and converted at the boundary
- [ ] All template parameters constrained with Concepts
- [ ] `[[nodiscard]]` on functions whose return value matters
- [ ] `std::filesystem::path` for all paths
- [ ] Platform `#ifdef` uses CMake-defined macros; `.hpp` is platform-agnostic
- [ ] No non-trivial global variables
- [ ] No raw owning pointers
- [ ] Tests written before or alongside implementation
- [ ] No new dependency without approval