# Lumen — Product Requirements Document

## Overview

Lumen is a structured, zero-instrumentation telemetry library for C++ with optional Python bindings. It is designed as a **core infrastructure library** reusable across high-energy physics analysis, ML experiment tracking, and game engine development. Its central proposition is that producers write the minimum possible — only semantic content — while consumers receive maximally rich, filterable records.

---

## Goals

- **Producer minimalism.** A log call should require only the message and level. All structural context is harvested automatically.
- **Consumer flexibility.** Sinks can subscribe to any subset of the record stream using composable tag-based predicates. Format, color, destination, and layout are fully configurable per sink.
- **Zero overhead in release.** Debug and trace records must compile to nothing in release builds via CMake flags. No formatting cost, no tag construction, no allocation.
- **Thread safety.** Producers on any thread may emit records concurrently without synchronization on the producer side.
- **Domain agnostic.** The same library, unmodified, must serve physics event loops, neural network training loops, and game frame loops. Domain-specific behavior lives in bridges and sinks, not in the core.
- **Extensible without modification.** Adding a new sink, bridge, or formatter must not require changes to core library files.

---

## Non-Goals

- Lumen is not a distributed tracing system. It does not implement span propagation across process or network boundaries (though a network sink may forward to one).
- Lumen is not a metrics database. In-process time series are kept as shallow circular buffers sufficient for live dashboard display; long-term storage is the responsibility of external sinks.
- Lumen does not parse or execute log queries at runtime. Predicates are registered at sink construction time and compiled to decision trees, not evaluated as query strings.
- Lumen is not a crash reporter. It does not install signal handlers or produce stack traces (though a sink may do so independently).

---

## Use Case Matrix

| Scenario | Record types used | Key automatic context | Key sink |
|---|---|---|---|
| ATLAS/HEP analysis | Log, Progress | run\_id, event\_id, subsystem, worker rank | Terminal, File, JSON |
| ML training | Log, Metric, Progress | epoch, batch, model name (via bridge) | Terminal dashboard, JSON |
| Game engine | Log, Metric, Progress | frame\_number, delta\_time, subsystem | Terminal, file per subsystem |
| Grid job (SLURM/HTCondor) | Log, Metric | job\_id, rank, hostname | File, JSON, network |
| Interactive analysis | Log | class, function, line | Colored terminal |

---

## Record Types

Lumen defines exactly three record types. This covers all use cases without conflating them.

**LogRecord** — a diagnostic message at a given severity level. The primary record type. Carries a human-readable message, a level, a full tag set, a timestamp, and a source location.

**MetricRecord** — a named scalar value observed at a point in time. Used for loss curves, frame rates, GPU memory, hit rates, or any quantity the consumer wants to track over time. Carries no message, only a name, value, tag set, and timestamp.

**ProgressRecord** — a completion state for a named job. Carries a stable identifier, a label, current and total counts, and a tag set. Multiple ProgressRecords with the same identifier represent updates to the same bar.

---

## Context Layers

Context is attached to records in five layers, evaluated at emit time and merged by priority (highest first):

1. **Explicit tags** — `.tag("k", "v")` on the emit call. Producer always wins.
2. **Instance tags** — tags registered by the enclosing object via `lumen::Tagged` mixin. Active for the object's lifetime.
3. **Scope stack** — tags pushed by `LUMEN_SCOPE`, `LUMEN_LOOP`, or `LUMEN_FRAME_SCOPE`. Active for the lexical scope.
4. **Thread tags** — tags registered once per thread at creation. Active for the thread's lifetime.
5. **Process tags** — tags registered once at startup. Active for the process lifetime.

When the same key appears at multiple layers, the higher-priority layer wins. This allows a producer to always override ambient context when needed without changing global state.

---

## Source Location

In C++20, `std::source_location::current()` is captured automatically at the macro call site — file, function name, and line number are attached to every LogRecord at zero runtime cost.

In C++26, static reflection (`std::meta`) will additionally provide the enclosing class name and full function signature. The library targets C++20 as the minimum and conditionally enables the richer C++26 path via a CMake feature flag.

---

## Release Build Elision

All emit macros expand conditionally based on CMake-defined log level thresholds. Below the threshold, the macro expands to a no-op expression that the compiler discards entirely — no formatting, no tag construction, no function call. This is equivalent to the existing approach in the author's current library.

```
LUMEN_MIN_LEVEL=INFO  →  DEBUG and TRACE calls compile to nothing
LUMEN_MIN_LEVEL=WARN  →  DEBUG, TRACE, INFO calls compile to nothing
```

MetricRecord and ProgressRecord emit calls are always included; they carry no debug/trace semantics.

---

## Bridges

A bridge is an optional header that, when included, registers hooks into a known framework and emits records automatically on behalf of the user. Bridges are the only mechanism for zero-instrumentation telemetry from third-party frameworks.

Planned bridges (not core, shipped as separate headers):

- `lumen/bridges/torch.h` — hooks into forward/backward passes, emits loss, gradient norms, learning rate as MetricRecords.
- `lumen/bridges/root.h` — hooks into TTree fills and histogram operations.
- `lumen/bridges/eigen.h` — emits timing for large matrix operations above a configurable size threshold.
- `lumen/bridges/openmp.h` — emits thread pool utilization as MetricRecords.

Bridges are entirely opt-in. A user without any bridge gets full functionality; bridges add automatic context at zero producer effort.

When no bridge exists for a framework, the user attaches context via explicit tags or instance tags. Both paths produce equivalent records — bridges are a convenience, not a requirement.

---

## Terminal Dashboard

The terminal sink renders a split view: a fixed dashboard region at the top (progress bars, metric sparklines, gauges) and a scrolling log region below. The dashboard region height is configurable.

The dashboard is driven entirely by the MetricRecord and ProgressRecord streams — no separate API. Any metric emitted to a terminal sink with a dashboard enabled will appear automatically.

The implementation uses FTXUI or notcurses for terminal multiplexing. This dependency is optional and gated behind a CMake flag; without it, the terminal sink renders log lines only.

---

## LaTeX Support

Records tagged with `latex=true` are treated by LaTeX-capable sinks as containing embedded math notation. The tag is inert for all other sinks — no cost, no behavior change.

A LaTeX sink renders math expressions using MicroTeX (a lightweight C++ math renderer) or by delegating to an external process. This feature is gated behind a CMake flag and is entirely optional. When the flag is off, the tag is ignored everywhere.

---

## Python Bindings

A Python interface is provided via pybind11. The Python API mirrors the C++ API in structure and emits into the same broker, enabling mixed C++/Python codebases (e.g., a PyTorch training loop calling into a C++ physics kernel) to share a single log stream and a single terminal dashboard.

Python-native sinks (e.g., integration with Python's `logging` module, or forwarding to `comet_ml`) are provided as Python-side sink implementations.

---

## Versioning and ABI Stability

The record types, broker interface, and sink base class form the stable ABI surface. Tag key/value strings are `std::string_view` pointing into caller-owned storage or into a per-record arena — no heap allocation on the hot path. The ABI is versioned via an inline namespace so multiple versions can coexist in the same binary during transition periods.
