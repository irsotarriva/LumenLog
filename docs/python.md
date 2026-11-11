# Python

The module `lumen_bindings` gives Python the same Core as C++: records
produced in Python and in C++ go through one queue to one set of sinks. It is
built with `-DLUMEN_ENABLE_PYTHON=ON` and needs pybind11 3.0 or later and
Python 3.11 or later. The CI job `python-bindings` builds it with Python 3.12.

```sh
cmake --preset debug -DLUMEN_ENABLE_DASHBOARD=OFF -DLUMEN_ENABLE_PYTHON=ON \
      -Dpybind11_DIR=$(python3 -m pybind11 --cmakedir)
cmake --build --preset debug
```

The module is `build/debug/python/lumen_bindings*.so`. Put that directory on
`PYTHONPATH`, and the repository's `python/` directory too for the `logging`
handler. In a sanitized build (the `debug` preset) the interpreter must
preload the sanitizer runtime; the ctest entries do that (see
`python/CMakeLists.txt`).

Every Python block on this page is run by ctest when the module is built
(`Docs.Python.*`).

## Example

`examples/python/quick_start.py`, run by ctest as `Example.Python`:

<!-- check: from=examples/python/quick_start.py -->
```python
"""Lumen from Python: a sink written in Python, a library sink, tags, a scope,
a metric, a progress bar, an error, and the standard logging module forwarded
to Lumen.

Needs the module built with LUMEN_ENABLE_PYTHON=ON. Run it with the directory
of lumen_bindings and the repository's python/ directory (for
logging_handler) on PYTHONPATH.
"""

import logging

import lumen_bindings as lumen
from logging_handler import LumenHandler


class Printer(lumen.Sink):
    """Prints each record on stdout. Called on the library's dispatch thread."""

    def on_log(self, record):
        tags = " ".join(f"{key}={value}" for key, value in record.tags.items())
        print(f"{record.level.name} {record.message} [{tags}] in {record.source.function}()")

    def on_metric(self, record):
        print(f"METRIC {record.name}={record.value} {record.tags}")

    def on_progress(self, record):
        print(f"PROGRESS {record.label} {record.current}/{record.total}")


def main():
    core = lumen.core()
    core.add_sink(Printer(), lumen.level_at_least(lumen.LogLevel.INFO))

    # A library sink, on stderr, for warnings and above.
    config = lumen.TerminalConfig()
    config.time_format = ""
    config.inline_tags = ["app", "phase"]
    core.add_sink(lumen.TerminalSink(config), lumen.parse_predicate("level >= WARN"))

    lumen.set_process_tag("app", "python_example")
    lumen.debug("below the Printer's level")
    with lumen.scope("phase", "setup"):
        lumen.info("configured", tags={"workers": 4})
        lumen.warn("disk almost full", tags={"free_gb": 1.5})
    lumen.metric("loss", 0.25, tags={"epoch": 3})

    bar = lumen.progress("files", 2)
    bar.tick()
    bar.tick()

    try:
        lumen.parse_predicate("level >= LOUD")
    except lumen.InvalidPredicateError as error:
        lumen.error(f"bad filter: {error}")

    logger = logging.getLogger("trainer")
    logger.setLevel(logging.INFO)
    logger.addHandler(LumenHandler())
    logger.info("epoch done", extra={"epoch": 3})

    core.flush()


if __name__ == "__main__":
    main()
```

On standard output (the `Printer` sink):

<!-- check: output=examples/expected/python_quick_start.stdout -->
```text
INFO configured [workers=4 phase=setup app=python_example] in main()
WARN disk almost full [free_gb=1.5 phase=setup app=python_example] in main()
METRIC loss=0.25 {'epoch': '3', 'app': 'python_example'}
PROGRESS files 1/2
PROGRESS files 2/2
ERROR bad filter: invalid predicate at offset 9: expected a level (TRACE, DEBUG, INFO, WARN, ERROR, FATAL) [app=python_example] in main()
INFO epoch done [logger=trainer epoch=3 app=python_example] in main()
```

On standard error (the `TerminalSink`, warnings and above):

<!-- check: output=examples/expected/python_quick_start.stderr -->
```text
WARN  disk almost full app=python_example phase=setup
METRIC loss=0.25 app=python_example
PROGRESS files 1/2 app=python_example
PROGRESS files 2/2 app=python_example
ERROR bad filter: invalid predicate at offset 9: expected a level (TRACE, DEBUG, INFO, WARN, ERROR, FATAL) app=python_example
```

## Producing records

| Function | Does |
|---|---|
| `trace`, `debug`, `info`, `warn`, `error` `(message, *, tags=None)` | Log `message` verbatim at that level |
| `log(level, message, *, tags=None, source=None)` | Log at `level`, `TRACE` to `ERROR`; `FATAL` raises `ValueError` |
| `fatal(message, *, tags=None)` | Log at `FATAL` and terminate the process (below) |
| `metric(name, value, *, tags=None)` | Record a metric |
| `progress(label, total)` | A `Progress` bar: `update(n)`, `tick()`, `finish()`, and the read-only `current`, `total`, `finished`; the same rules as `ProgressHandle` in C++ |
| `now_ns()` | The clock records are stamped with |

- **Tags** are a `dict` of `str` keys to `str`, `bool`, `int` (or anything with
  `__index__`, such as a NumPy integer) or `float` values. Numbers are written
  as text exactly as C++ `.tag()` writes them, so a predicate sees the same
  text whichever language logged the record. Another value type raises
  `TypeError`; an `int` outside both the int64 and the uint64 range raises
  `OverflowError`.
- **Source.** A log record carries the calling Python file, line and
  qualified function name. `class_name` is the part of the qualified name
  before the function's own (`Trainer.step` gives `Trainer`), and empty for a
  function nested in a function. `set_source_capture(False)` turns this off
  (`source_capture()` reads it); `log(..., source=(file, line, function))`
  records a given location instead.
- **Level threshold.** The C++ compile-time threshold does not apply: every
  Python log call produces a record. Filter with predicates.

## Context

`set_thread_tag`, `remove_thread_tag`, `clear_thread_tags`, `set_process_tag`,
`remove_process_tag` and `clear_process_tags` work as in C++. The value is a
`str`, `bool`, `int` or `float`. A new key in a full table raises
`TagTableFullError`.

`with lumen.scope(key, value):` adds a scope tag to the records of the
calling thread inside the block. Scopes belong to a thread, not to an asyncio
task or a generator: leaving a scope also closes every scope entered after it
on the same thread, leaving one on another thread raises `RuntimeError`, and
entering a `Scope` object that is already entered raises `ValueError`. A scope
still open when its thread ends is closed then.

```python
import lumen_bindings as lumen

seen = []
sink_id = lumen.core().add_sink(lumen.CallbackSink(on_log=lambda r: seen.append(r.tags)))

lumen.set_thread_tag("worker", 1)
with lumen.scope("run", 7):
    lumen.info("inside", tags={"run": 8})   # the explicit tag wins
with lumen.scope("run", 7):
    lumen.info("inside")
lumen.info("outside")
lumen.core().flush()
lumen.core().remove_sink(sink_id)

assert seen == [{"run": "8", "worker": "1"}, {"run": "7", "worker": "1"}, {"worker": "1"}], seen

lumen.clear_thread_tags()
for i in range(8):
    lumen.set_thread_tag(f"key{i}", i)
try:
    lumen.set_thread_tag("one_too_many", 0)   # a thread holds 8 keys
except lumen.TagTableFullError:
    pass
else:
    raise AssertionError("expected TagTableFullError")
```

## Sinks

- **`lumen.Sink`**: subclass it and define any of `on_log(record)`,
  `on_metric(record)`, `on_progress(record)` and `flush()`; the others do
  nothing. A subclass with its own `__init__` must call `super().__init__()`.
- **`lumen.CallbackSink(on_log=None, on_metric=None, on_progress=None,
  flush=None)`**: the same from plain callables.
- **`lumen.TerminalSink(config)`** with a `lumen.TerminalConfig` (`color`,
  `colors`, `time_format`, `time_zone`, `inline_tags`, `show_source`,
  `max_queued_lines`); lines go to standard error. **`lumen.FileSink(config)`**
  with a `lumen.FileConfig(path, *, format, max_file_bytes, max_files,
  max_queued_lines)`. Both have `statistics()`; `FileSink` has `path`.
  **`lumen.NullSink()`**. The `output` stream and `diagnostics` handler of
  the C++ configurations are not available from Python.
- Register with `lumen.core().add_sink(sink, predicate)`; it returns the id.
  The sink then belongs to the library until `remove_sink(id)` returns it.

Records reach Python as read-only copies (`LogRecord`, `MetricRecord`,
`ProgressRecord`, with `SourceLocation`) that a sink may keep. `tags` is a new
`dict` on every access, its values `str`.

An exception raised by a Python sink method is counted
(`Core.sink_exception_count(id)`) and the first one per sink is reported
through the diagnostic handler; delivery to the other sinks continues.

## Predicates

`always`, `never`, `level_at_least`, `level_equals`, `tag_equals`,
`tag_exists`, `tag_less`, `tag_less_equal`, `tag_greater`,
`tag_greater_equal` and `parse_predicate` build predicates, combined with `&`,
`|` and `~`. `tag_not_equals`, `level_compare` and `tag_compare` are not
bound; `parse_predicate` covers them (`run != 7`, `level < WARN`).
`Predicate.evaluate(tags, level=None)` tells whether a record with these tags
would pass: with a level as a log record, without one as a metric or
progress record. The rules are those of [Predicates](predicates.md).

```python
import lumen_bindings as lumen

p = lumen.parse_predicate("level >= WARN || run == 42")
assert p.evaluate({"run": 42})                       # no level: a metric; level is ignored
assert not p.evaluate({"run": 7})
assert p.evaluate({}, lumen.LogLevel.ERROR)
assert not (p & lumen.tag_exists("missing")).evaluate({"run": 42})
assert (~lumen.tag_equals("k", "v")).evaluate({})   # negation of a false comparison

try:
    lumen.parse_predicate("level >= LOUD")
except lumen.InvalidPredicateError as error:
    assert "offset 9" in str(error), error
```

## The Core

`lumen.core()` returns the process-wide Core; it cannot be created from
Python. It has `add_sink`, `remove_sink`, `sink_exception_count`, `flush`,
`statistics()` (a `CoreStatistics` with `logs`, `metrics`, `progress` and
`tags_dropped`), `set_diagnostic_handler(callable or None)` and
`reset_diagnostic_handler()`. A Python diagnostic handler is called with one
`str` on the library's dispatch thread; an exception it raises goes to
`sys.unraisablehook`.

## Errors

Library errors are raised as subclasses of `lumen.LumenError` (itself a
`RuntimeError`): `InvalidSinkIdError` (also a `LookupError`),
`SinkBusyError`, `TagTableFullError`, `InvalidPredicateError` and
`InvalidSinkConfigError` (both also `ValueError`), `FileOpenError` (also an
`OSError`). An operating-system error is raised as `OSError`; a wrong
argument type as `TypeError`.

## Threads and the GIL

- The producers keep the GIL. Submitting a record takes no lock that the
  dispatch thread holds while it waits for the GIL, so they cannot deadlock
  with it.
- A Python sink's methods run on the dispatch thread (or on the thread that
  calls `Core.flush`), each taking the GIL for the duration of the call. A
  slow Python sink delays every other sink of the Core, C++ ones included,
  and competes with the interpreter's own threads for the GIL.
- `Core.flush()` releases the GIL while it delivers, so Python sinks can run
  meanwhile. `add_sink` for a C++ sink and `remove_sink` release it too.
- `fatal()` releases the GIL before it delivers, so Python sinks receive the
  fatal record.

## Exit

The global Core outlives the interpreter (it shuts down at C++ static
destruction). At import the module registers an `atexit` function that runs
while the interpreter is still whole, after every `atexit` function the
program registers later. It refuses new Python sinks (`add_sink` raises
`RuntimeError`), delivers everything queued so far, removes and destroys the
Python sinks, puts back the default diagnostic handler if Python had
replaced it, and from then on no library thread calls into Python. Sinks
written in C++ keep receiving records until the process ends. This also
covers an unhandled exception and `sys.exit()`. `os._exit()` and a fatal
signal skip it, and C++ static destruction too.

`fatal()` terminates the process with `std::terminate()` (SIGABRT) after
delivering and flushing: `finally` blocks and `atexit` functions do not run.

## The `logging` handler

`python/logging_handler.py` defines `LumenHandler`, a `logging.Handler` that
forwards each `logging.LogRecord` as one Lumen log record:

- message: the handler's formatted text (by default the message, plus the
  traceback when there is `exc_info`);
- level: the highest Lumen level at or below the record's level. `DEBUG`,
  `INFO`, `WARNING` and `ERROR` map to `DEBUG`, `INFO`, `WARN` and `ERROR`;
  anything below `DEBUG` to `TRACE`; `CRITICAL` to `ERROR`, never to
  `FATAL`, because a logging call must not terminate the process. A record
  whose level is not one of those four standard levels also gets the tag
  `logging.level` with its level name;
- source: the record's `pathname`, `lineno` and `funcName`;
- tags, in this order: `logger` with the logger's name, `logging.level`
  (above), then every `extra` field in the order given. A `str`, `bool` or
  `float` is passed as it is, and an `int` too if it fits in int64 or uint64;
  anything else, a larger `int` included, as `str(value)`. An `extra` field
  whose name an earlier tag already has is passed with `extra.` put in front
  of its name (as often as it takes to be new), so `extra={"logger": ...}`
  becomes the tag `extra.logger`. Tags beyond 16 are dropped and counted
  (`tags_dropped`); the handler's own tags come first, so they are kept.
  `lumen_tags(record)` returns the tags a record is forwarded with.

An error while forwarding goes to `logging`'s own `handleError()`.

```python
import logging

import lumen_bindings as lumen
from logging_handler import LumenHandler, lumen_level

assert lumen_level(logging.WARNING) == lumen.LogLevel.WARN
assert lumen_level(logging.CRITICAL) == lumen.LogLevel.ERROR
assert lumen_level(5) == lumen.LogLevel.TRACE

seen = []
sink_id = lumen.core().add_sink(lumen.CallbackSink(on_log=lambda r: seen.append(r)))
logger = logging.getLogger("trainer")
logger.addHandler(LumenHandler())
logger.critical("disk failed", extra={"disk": 2})
lumen.core().flush()
lumen.core().remove_sink(sink_id)

assert seen[0].level == lumen.LogLevel.ERROR
assert seen[0].tags == {"disk": "2", "logger": "trainer", "logging.level": "CRITICAL"}, seen[0].tags
assert seen[0].source.function == "<module>"
```
