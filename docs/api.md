# Headers

The public headers are the API reference: each one states, next to every
declaration, what it does, on which thread and with which errors. This page
says where to look. `lumen/lumen.hpp` includes every header in the first
table; the others are included on their own.

<!-- check: header-table -->
| Header | Declares |
|---|---|
| `lumen/lumen.hpp` | Includes the headers below that a program that logs needs |
| `lumen/macros.hpp` | `LUMEN_LOG_TRACE` … `LUMEN_LOG_FATAL`, `LOG_*`, `LUMEN_SCOPE`, `LUMEN_MEMBER_SCOPE`, `LUMEN_LOOP`, `level_compiled_in` |
| `lumen/core.hpp` | `Core` (`add_sink`, `remove_sink`, `flush`, `statistics`, `sink_exception_count`, `set_diagnostic_handler`), `core()`, `CoreStatistics`, `QueueStatistics`, `DiagnosticHandler`, `stderr_diagnostics()`; the threading, ordering, loss and lifetime rules |
| `lumen/record_builder.hpp` | `RecordBuilder`, `MetricBuilder`, `metric()`; tag rules; what a fatal record does |
| `lumen/progress.hpp` | `ProgressHandle`, `progress()` |
| `lumen/context.hpp` | `set_process_tag`, `set_thread_tag` and their `remove_*` and `clear_*`; the priority of the context layers |
| `lumen/tagged.hpp` | `Tagged` (`lumen_tag`, `lumen_untag`, `lumen_tags`, `lumen_scope`) |
| `lumen/record.hpp` | `LogRecord`, `MetricRecord`, `ProgressRecord`, `SourceLocation`, `RecordTags`; who owns a record's strings |
| `lumen/tag_set.hpp` | `TagSet<N>`, the fixed-capacity tag list of a record |
| `lumen/level.hpp` | `LogLevel` |
| `lumen/limits.hpp` | Every fixed capacity, and what happens when it is reached |
| `lumen/predicate.hpp` | `Predicate`, its builders and operators, `parse_predicate` and its grammar, `PredicateParseError` |
| `lumen/error.hpp` | `LumenError`, its `std::error_code` category |
| `lumen/sink.hpp` | `Sink` and its contract, `NullSink` |
| `lumen/terminal_sink.hpp` | `TerminalSink` and its `Config`; the line layout |
| `lumen/file_sink.hpp` | `FileSink`, its `Config`, `FileFormat` and the three line formats; rotation |
| `lumen/thread_clock.hpp` | `now_ns()`, `this_thread_id()`: the timestamp and thread number of records |

| Header | Declares |
|---|---|
| `lumen/terminal_style.hpp` | `AnsiColor`, `ColorMode` (and the colour rule of `Auto`), `LevelColors`, `default_level_colors`, `color_for`, `TimeZone` |
| `lumen/writer_statistics.hpp` | `WriterStatistics`; how the shipped sinks queue and write lines |
| `lumen/dashboard_sink.hpp` | `DashboardSink` (only linkable with `LUMEN_ENABLE_DASHBOARD=ON`) |
| `lumen/dashboard_model.hpp` | `DashboardModel`, what the dashboard keeps; usable without FTXUI |
| `lumen/bridges/eigen.hpp` | `emit_matrix_summary`, `emit_solver_summary` ([Bridges](bridges.md)) |
| `lumen/bridges/openmp.hpp` | `emit_openmp_configuration`, `OpenMpWorkBalance` |
| `lumen/bridges/root.hpp` | `emit_tree_summary` |
| `lumen/bridges/torch.hpp` | `TorchTrainingMetrics`, `TorchMetricsError` |

Everything is in namespace `lumen`, the bridges in `lumen::bridges`. Headers
under `lumen/detail/` are internal: they are included by the public headers
and are not part of the API.

Every public header compiles as the only include of a translation unit
(test `CompileCheck.EveryHeaderAlone`), and every bridge header compiles
without its framework (`CompileCheck.BridgesWithoutFrameworks`).

## Error codes

`LumenError` values are `std::error_code`s of the category `"lumen"`.
Compare a returned code with the enumerator directly:

<!-- check: prelude=common wrap=body -->
```cpp
const auto set = lumen::set_thread_tag("stage", "warmup");
if (!set && set.error() == lumen::LumenError::TagTableFull) {
    LUMEN_LOG_WARN("too many thread tags; stage not set");
}
```

| Code | Returned by |
|---|---|
| `InvalidSinkId` | `remove_sink`, `sink_exception_count`: no sink has this id |
| `NullSink` | `add_sink`: the sink is null |
| `SinkBusy` | `remove_sink` from inside a callback of the sink being removed |
| `TagTableFull` | `set_process_tag`, `set_thread_tag`, `Tagged::lumen_tag`: the key is new and the table is full |
| `InvalidPredicate` | `parse_predicate` (in `PredicateParseError::code`) |
| `InvalidSinkConfig` | `TerminalSink::create`, `FileSink::create`, `DashboardSink::create`: a `Config` field is out of range |
| `FileOpenFailed` | `FileSink::create`: the file cannot be opened for appending |
| `IoError` | never returned: a failed write is reported through a sink's diagnostics and counted in its statistics |
| `NotATerminal` | `DashboardSink::create`: the destination is not a terminal |
| `CoreShutDown` | `add_sink` once the Core has begun shutting down |
