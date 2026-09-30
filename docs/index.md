# Lumen Documentation

Lumen is a structured, zero-instrumentation telemetry library for C++20. It separates the act of producing diagnostic data from the act of consuming it — producers write only what they know, consumers receive everything they need.

## How to read these docs

If you're new, start with [Getting Started](getting-started.md). If you want to understand *why* Lumen works the way it does, read [Philosophy](philosophy.md).

| Document | What you'll learn |
|---|---|
| [Getting Started](getting-started.md) | Requirements, installation, your first program |
| [Philosophy](philosophy.md) | The producer-consumer model, context layers, design rationale |
| [Usage Guide](usage.md) | Log calls, metrics, progress bars, scope helpers, tags |
| [Sinks](sinks.md) | Terminal, file, JSON sinks. Configuration. Writing custom sinks. |
| [Predicates](predicates.md) | Filtering records with the predicate DSL |
| [API Reference](api.md) | Every public type, function, and macro |
| [Bridges](bridges.md) | Zero-instrumentation hooks for PyTorch, ROOT, Eigen, OpenMP |

## Architecture at a glance

```
LOG_INFO("msg")    ──►  5-layer context merge   ──►  lock-free ring buffer
                                                          │
lumen::metric()    ──►  record-owned strings         dispatch thread
                                                          │
lumen::progress()  ──►  compile-time elision         predicate evaluation
                                                          │
                                                    TerminalSink   FileSink   JsonSink
```

Producers never allocate on the hot path. Records flow through lock-free MPSC ring buffers. A single dispatch thread evaluates predicates and routes matching records to sinks. Sinks handle I/O on their own threads.
