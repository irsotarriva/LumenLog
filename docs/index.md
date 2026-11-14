# Lumen documentation

Lumen records log messages, metrics and progress bars as structured records
with key/value tags, and delivers them from one background thread to sinks
that each choose records with a predicate. It is a C++23 library with an
optional Python module.

## The model

- A **producer** is any statement that makes a record: `LUMEN_LOG_INFO(...)`,
  `lumen::metric(...)`, a progress handle, a `LUMEN_LOOP`. It names the
  message or value and, if it wants, some tags.
- **Context** adds tags the statement does not name: the process's, the
  thread's, those of the enclosing `LUMEN_SCOPE`s and of the
  `lumen::Tagged` object whose member function is running. A library deep in
  the call stack logs with the run number that the caller set once.
- The **Core** queues the record and delivers it, on its own thread, to every
  **sink** whose **predicate** accepts it. Sinks decide the format and the
  destination; adding a sink changes no logging statement.

Producers pay for what they record: building the record, copying its strings
into one heap block and one push onto a queue. Statements below the
compile-time level threshold cost nothing at all. The [Design](design.md) page
says what happens on each thread.

## Pages

| Page | Contents |
|---|---|
| [Getting started](getting-started.md) | Building Lumen, using it from your project, the first program |
| [Usage](usage.md) | Log statements, tags, compile-time level, metrics, progress, loops, context, fatal, statistics |
| [Predicates](predicates.md) | Building and parsing filters; the rules; how they are evaluated |
| [Sinks](sinks.md) | The sink contract; terminal, file and dashboard sinks and their formats; writing your own |
| [Python](python.md) | The Python module, its threading and exit behaviour, the `logging` handler |
| [Bridges](bridges.md) | Helpers that emit metrics from Eigen, OpenMP, ROOT and libtorch objects |
| [Headers](api.md) | Which header declares what; error codes |
| [Design](design.md) | The architecture as it is, for maintainers |

The examples in `examples/` are complete programs. ctest runs each one and
compares its output with `examples/expected/`; the pages quote both, and the
documentation check (`tests/docs/check_docs.py`) fails when a quote and the
file differ.
