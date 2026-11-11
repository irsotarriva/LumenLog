# Design

How Lumen works inside, as of version 0.1.0: a map for maintainers. The
headers state the guarantees; this page says which file keeps them.

## Files

| File | Owns |
|---|---|
| `include/lumen/core.hpp`, `src/core.cpp` | `Core`: the queues, the dispatch thread, the sink registry, `flush`, fatal records, shutdown, statistics, diagnostics |
| `include/lumen/detail/ring_buffer.hpp` | `RingBuffer`, the bounded multi-producer single-consumer queue |
| `include/lumen/record_builder.hpp`, `include/lumen/detail/string_scratch.hpp` | Builders and their scratch buffer for strings |
| `include/lumen/macros.hpp` | The logging front end: statements, compile-time elision, scopes, loops |
| `include/lumen/context.hpp`, `include/lumen/detail/context.hpp`, `src/context.cpp` | All context state: the process table, each thread's tags and scope frames, and the merge into a record |
| `include/lumen/detail/scope_stack.hpp` | `ScopeGuard` and `LoopScope`, behind `LUMEN_SCOPE` and `LUMEN_LOOP` |
| `include/lumen/tagged.hpp`, `src/tagged.cpp` | `Tagged` objects and their member scopes |
| `include/lumen/detail/class_name.hpp` | The enclosing class, read from `std::source_location::function_name()` |
| `include/lumen/progress.hpp`, `src/progress.cpp` | `ProgressHandle` |
| `include/lumen/predicate.hpp`, `src/predicate.cpp` | Predicate trees and their evaluation |
| `src/predicate_lexer.*`, `src/predicate_parser.cpp`, `src/number_parse.hpp`, `src/ascii.hpp` | `parse_predicate`, locale-independent |
| `src/record_format.*`, `src/text_encoding.*`, `src/time_format.*` | The line formats of the shipped sinks |
| `src/line_writer.*` | The writer thread and bounded line queue shared by `TerminalSink` and `FileSink` |
| `src/terminal_output.*`, `src/terminal_sink.cpp` | `TerminalSink`, its destination and the colour decision |
| `src/rotating_file.*`, `src/file_sink.cpp` | `FileSink`: `{date}`, appending, rotation |
| `src/dashboard_model.cpp` | `DashboardModel` (always built) |
| `src/dashboard_sink.cpp`, `src/dashboard_frame.*` | `DashboardSink`; the only code that uses FTXUI (built with `LUMEN_ENABLE_DASHBOARD`) |
| `src/immortal.hpp` | Storage for the few objects that are never destroyed |
| `src/error.cpp` | The `"lumen"` error category |
| `python/` | The pybind11 module, one file per area (`python/registration.hpp` lists them), and `logging_handler.py` |
| `cmake/build_settings.cmake`, `cmake/config.hpp.in` | The settings that change what the headers compile to (`LUMEN_MIN_LEVEL`, queue capacities), written into the generated and installed `lumen/config.hpp` |

## One submission path

Every record enters through `Core::submit_` (private; the builders and
`ProgressHandle` are friends). On the producing thread it:

1. returns at once, counting the record in `dropped_after_shutdown`, if the
   Core no longer accepts records;
2. stamps the record: `now_ns()` and `this_thread_id()`;
3. merges the context (`detail::merge_context`, below) after the explicit
   tags, and counts the tags that did not fit;
4. copies every string the record points to (message or name or label, every
   tag key and value, and a log record's source if it was given as text) into
   one immutable block, `std::shared_ptr<const char[]>`, and re-points the
   views at it. That is the one allocation per record;
5. pushes the record into the queue of its type and, if the dispatcher is
   asleep, wakes it.

A builder collects the explicit strings before step 4 in a 512-byte buffer on
its own stack (`StringScratch`); longer strings go to heap nodes. So records
never point into producer memory, and a sink may keep a copy of a record for
as long as it likes.

Fatal records take a different branch (`submit_fatal_`): steps 1 to 4, then
the calling thread delivers everything queued, the fatal record and a flush,
and terminates.

## Queues

Three `RingBuffer`s, one per record type, with capacities fixed at compile
time (`lumen/limits.hpp`, powers of two). The queue is D. Vyukov's bounded
queue with the consumer side specialised to one thread: a producer claims a
position with a CAS on the head counter and publishes the slot through its
sequence number; a full queue refuses the record and counts it
(`rejected_count`, reported as `dropped_queue_full`). There is no blocking or
overwriting mode. The comment at the top of
`include/lumen/detail/ring_buffer.hpp` gives the full protocol and the memory
orders.

## The dispatcher

One thread per Core. Each pass reads the three head positions and delivers,
up to them, the progress records, then the metrics, then the logs: within a
type in queue order, across types not in time order. For each record it walks
a snapshot of the registry and calls every sink whose predicate accepts it.

Wake-up. After a pass that delivered something, the dispatcher checks the
queues five more times, 100 µs apart, before it sleeps, because records tend
to come in bursts. To sleep it sets an atomic flag, checks the queues once
more and waits on a condition variable. A producer, after pushing, clears the
flag with an atomic exchange and notifies only if it was set: every record
costs one atomic exchange, and only a producer that finds the dispatcher
asleep pays for the mutex and the notify. The comment on `wake_if_sleeping`
in `src/core.cpp` argues why no record can wait for a sleeping dispatcher.

`Core::flush()` takes the same consume mutex as the dispatcher and delivers
on the caller's thread up to the head positions it read, waiting for
producers that had claimed a slot before it, then flushes every sink.

## The sink registry

A vector of shared entries (sink, predicate, counters), guarded by a mutex
and versioned. The delivering thread keeps a snapshot and refreshes it only
when the version changed, so delivering a record takes no lock. Removal uses
a handshake of two atomics per entry: the deliverer sets `busy` and then
reads `removed` before each callback, `remove_sink` sets `removed` and then
waits while `busy`. A thread-local records which Core and sink the current
thread is delivering for, so that `flush`, `remove_sink` and fatal records
can recognise a call from inside a callback and not wait for themselves.
Exceptions from callbacks are caught in one place (`call_sink`), counted per
sink and reported once.

## Context

`src/context.cpp` owns every piece of context state. The process table is one
object, never destroyed, guarded by a mutex, with a version counter that grows
on every change. Each thread owns a `thread_local` context: its thread tags,
the heads of two intrusive lists of frames (scope frames from `LUMEN_SCOPE`
and `LUMEN_LOOP`, instance frames from `LUMEN_MEMBER_SCOPE`), and its own copy
of the process table, refreshed under the mutex only when the version moved.
A guard pushes its frame when constructed and pops it when destroyed; debug
builds assert that this happens on the same thread in stack order.

The merge walks instance frames (reading each object's tags at that moment),
then scope frames, then the thread tags, then the thread's copy of the
process tags, and adds each key the record does not have until the record
holds 16 tags. It runs on the producing thread and reads only memory that no
other thread writes.

## Sinks that write lines

`TerminalSink` and `FileSink` share `LineWriter`: `on_*` formats the line
(`record_format`) on the dispatch thread and appends it to a bounded queue
under a mutex; the writer thread swaps the whole queue out, writes the batch,
flushes the destination and counts what it wrote. `flush()` waits for the
writer to pass the position that was queued when it was called.
`DashboardSink` instead keeps a `DashboardModel` under a mutex; its thread
copies the model and draws from the copy.

## Lifetime

`lumen::core()` keeps the Core in `Immortal` storage, never destroyed, and a
function-local static `ShutdownAtExit` whose destructor shuts the Core down.
That destructor runs where a plain function-local static Core would have been
destroyed. Shutdown stops accepting records, stops the dispatcher, delivers
what is queued, flushes and destroys the sinks. Later records are counted in
`dropped_after_shutdown`; the Core object itself stays valid, so nothing
touches destroyed memory. The process context is never destroyed either.

The Python module has to detach its sinks earlier, while the interpreter is
still running: an `atexit` function (`python/interpreter_exit.cpp`) delivers
what is queued, removes the Python sinks, restores the default diagnostic
handler and closes a gate that every later call into Python checks.
