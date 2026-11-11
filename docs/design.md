# Design

How Lumen works inside, as of version 0.1.0: a map for maintainers. The
headers state the guarantees; this page says which file keeps them.

## Files

<!-- check: files-table -->
| File | Owns |
|---|---|
| `include/lumen/core.hpp`, `src/core.cpp` | `Core`: its API, and `Core::Impl`, which composes the parts below and runs the sequences that span them (submission, the dispatch loop, `flush`, fatal records, shutdown, `fork()`) |
| `include/lumen/diagnostics.hpp` | `DiagnosticHandler` and `stderr_diagnostics()`, shared by the Core and the sinks |
| `src/dispatcher_thread.*` | The dispatch thread as a thread: starting it, and waiting for it to end with a limit |
| `src/flush_requests.*` | The flushes `flush_for` asks the dispatch thread to make, and the waits for them |
| `src/fork_handlers.*` | The one `pthread_atfork` registration and the objects it calls around `fork()` |
| `src/termination.*` | How the process ends after a fatal record, and how a terminate handler of Lumen's hands over |
| `src/portable_error.hpp` | Thread-start errors as `std::generic_category` codes on every standard library |
| `src/record_queues.*` | The three queues of a Core and the per-type count of records refused after shutdown |
| `src/record_preparation.hpp` | Stamping a submitted record, merging the context and copying its strings into its own block |
| `src/wakeup.*` | How producers wake the sleeping dispatch thread, and how it waits |
| `src/sink_registry.*` | The registered sinks, their removal handshake and the registry's versions |
| `src/delivery.*`, `src/delivery_stack.*` | Handing records to sinks under the delivery lock; the per-thread stack of deliveries in progress and their records in flight |
| `src/fatal_records.*` | Fatal records: announcing, posting and delivering them all before termination |
| `src/diagnostic_reporter.*`, `src/first_reports.*` | Passing diagnostic lines to the handler one at a time; "report the first, then count" for queue drops, tag drops and sink exceptions |
| `include/lumen/detail/ring_buffer.hpp` | `RingBuffer`, the bounded multi-producer single-consumer queue |
| `include/lumen/record.hpp`, `include/lumen/tag_set.hpp`, `include/lumen/level.hpp` | The three record types, their fixed-capacity tag list and the log level |
| `include/lumen/limits.hpp`, `include/lumen/thread_clock.hpp` | Every fixed capacity; the timestamp and thread number of a record |
| `include/lumen/sink.hpp` | The `Sink` contract and `NullSink` |
| `include/lumen/lumen.hpp` | Includes the headers a program that logs needs |
| `include/lumen/record_builder.hpp`, `include/lumen/detail/string_scratch.hpp` | Builders and their scratch buffer for strings |
| `include/lumen/macros.hpp` | The logging front end: statements, compile-time elision, scopes, loops |
| `include/lumen/context.hpp`, `include/lumen/detail/context.hpp`, `src/context.cpp` | All context state: the process table, each thread's tags and scope frames, and the merge into a record |
| `include/lumen/detail/tag_table.hpp` | `TagTable`, the owned tags of the process, a thread or a `Tagged` object |
| `include/lumen/detail/scope_stack.hpp` | `LoopScope`, behind `LUMEN_LOOP` (`LUMEN_SCOPE` declares a `ScopedTag`, `include/lumen/context.hpp`) |
| `include/lumen/tagged.hpp`, `src/tagged.cpp` | `Tagged` objects and their member scopes |
| `include/lumen/detail/class_name.hpp` | The enclosing class, read from `std::source_location::function_name()` |
| `include/lumen/progress.hpp`, `src/progress.cpp` | `ProgressHandle` |
| `include/lumen/predicate.hpp`, `src/predicate.cpp` | Predicate trees and their evaluation |
| `src/predicate_lexer.*`, `src/predicate_parser.cpp`, `src/number_parse.hpp`, `src/ascii.hpp` | `parse_predicate`, locale-independent |
| `src/record_format.*`, `src/text_encoding.*`, `src/time_format.*` | The line formats of the shipped sinks |
| `include/lumen/writer_statistics.hpp`, `src/line_writer.*` | The writer thread and bounded line queue shared by `TerminalSink` and `FileSink`, and their counters |
| `include/lumen/terminal_style.hpp` | Colours and time zone, shared by `TerminalSink` and `DashboardSink` |
| `include/lumen/terminal_sink.hpp`, `src/terminal_output.*`, `src/terminal_sink.cpp` | `TerminalSink`, its destination and the colour decision |
| `include/lumen/file_sink.hpp`, `src/rotating_file.*`, `src/file_sink.cpp` | `FileSink`: `{date}`, appending, rotation |
| `include/lumen/dashboard_model.hpp`, `src/dashboard_model.cpp` | `DashboardModel` (always built) |
| `include/lumen/dashboard_sink.hpp`, `src/dashboard_sink.cpp`, `src/dashboard_frame.*` | `DashboardSink`; the only code that uses FTXUI (built with `LUMEN_ENABLE_DASHBOARD`) |
| `include/lumen/detail/immortal.hpp` | Storage for the few objects that are never destroyed (also used by the Python module) |
| `include/lumen/error.hpp`, `src/error.cpp` | `LumenError` and the `"lumen"` error category |
| `include/lumen/bridges/` | The header-only bridges ([Bridges](bridges.md)) |
| `python/` | The pybind11 module, one file per area (`python/registration.hpp` lists them), and `logging_handler.py`; `python/interpreter_exit.*` the exit and fork hooks, `python/python_diagnostics.*` diagnostic handlers written in Python |
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
   views at it. That is the one allocation per record (each string longer
   than what is left of the builder's buffer adds two, made by the builder;
   `lumen/core.hpp`, "Cost of a statement", lists the costs);
5. pushes the record into the queue of its type and, if the dispatcher is
   asleep, wakes it.

A builder collects the explicit strings before step 4 in a 512-byte buffer on
its own stack (`StringScratch`); longer strings go to heap nodes. So records
never point into producer memory, and a sink may keep a copy of a record for
as long as it likes.

Fatal records take a different branch (`submit_fatal_`): the record is
counted as announced, goes through steps 1 to 4 and is posted to a small
mailbox instead of the queue. The thread that holds the delivery lock (the
caller, or the thread already delivering if the fatal record was raised in a
callback) first finishes the record whose callback was running, then
delivers everything queued and every posted fatal record, waits until every
announced fatal record has been posted and delivered, then 20 ms more for
fatal records raised at about the same time, flushes the sinks and
terminates. So fatal records raised on several threads at once are all
delivered.

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
more and waits on a condition variable. A producer, after pushing, reads the
flag and, only if it is set, clears it with an atomic exchange and notifies:
every record costs one atomic load of a cache line that nobody writes while
the dispatcher is awake, and only a producer that finds the dispatcher asleep
pays for the exchange, the mutex and the notify. The comment on `Wakeup` in
`src/wakeup.hpp` argues why no record can wait for a sleeping dispatcher.

`Core::flush()` takes the same delivery lock as the dispatcher and delivers
on the caller's thread up to the head positions it read, waiting for
producers that had claimed a slot before it, then flushes every sink.
`Core::flush_for()` instead takes a ticket (`src/flush_requests.hpp`) and
wakes the dispatcher, whose next pass reads the latest ticket, delivers up
to the heads it reads after it, flushes the sinks and completes every ticket
up to that one; the caller waits on a condition variable with its deadline,
so giving up never leaves it inside a callback.

Cycles. A thread inside a callback of Core A that waits for Core B (for B's
delivery lock in `flush()`, or for B's dispatcher in `flush_for()`) marks
every Delivery it holds as waiting for B, and every 5 ms follows the marks
from B: reaching a Delivery it holds itself means the wait would never end,
and the call returns `SinkBusy` (`Delivery::Waiting` in `src/delivery.hpp`).

## The sink registry

A vector of shared entries (sink, predicate, counters), guarded by a mutex
and versioned. The delivering thread keeps a snapshot and refreshes it only
when the version changed, so delivering a record takes no lock. Removal uses
a handshake of two atomics per entry: the deliverer sets `busy` and then
reads `removed` before each callback, `remove_sink` sets `removed` and then
waits while `busy`. A thread-local stack of delivery frames records which
Cores the current thread is delivering for (a sink of one Core may flush
another) and, per frame, the record being handed out and the next sink to
offer it to. With it `flush`, `remove_sink` and fatal records recognise a
call from inside a callback and do not wait for themselves, and a fatal
record or a shutdown started in a callback can finish the record in flight.
Exceptions from callbacks are caught in one place (`call_sink` in
`src/delivery.cpp`), counted per sink and reported once.

Diagnostic lines go through one queue per Core (`src/diagnostic_reporter.*`):
the thread that finds nobody passing lines to the handler does so until the
queue is empty, and the others only add their line, so the handler never
runs on two threads at once. The reporter holds no lock while the handler
runs; the line about a sink's exception is reported by the delivering
thread, which holds the delivery lock then. A handler that throws has its
line and the exception written to `std::cerr`.

## Context

`src/context.cpp` owns every piece of context state. The process table is
copied on write: a change, under a mutex, publishes a new immutable table
through a `shared_ptr` and increments a version counter; the object that
holds them is never destroyed. Each thread owns a `thread_local` context: its
thread tags, two stacks of frames (scope frames from `LUMEN_SCOPE` and
`LUMEN_LOOP`, which hold copies of their key and value, and instance frames
from `LUMEN_MEMBER_SCOPE`, which point at their object), and a `shared_ptr`
to the process table it last read, replaced under the mutex only when the
version moved. A guard pushes its frame when constructed and pops it when
destroyed; debug builds assert that this happens on the same thread in stack
order, and `lumen/detail/context.hpp` states what release builds do
otherwise.

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
destroyed. Shutdown stops accepting records and sinks, stops the
dispatcher, delivers what is queued, flushes, and destroys the sinks after
releasing the registry lock (a sink's destructor may call the Core). Later
records are counted in `dropped_after_shutdown` without reading the context;
the Core object itself stays valid. Whether a record submitted earlier in
static or thread-local destruction can read its thread's context is
`src/context.cpp`'s concern.

A shutdown that starts inside a sink callback (the callback called `exit()`)
cannot join the dispatcher, which is the calling thread or waits for the
delivery lock that thread holds, and cannot destroy sinks that are in a call
on its stack. It delivers what is left on that thread, flushes, detaches the
dispatcher and keeps the sinks.

If the dispatch thread cannot be started, the Core reports it and delivers
each record on the submitting thread.

Shutdown, and a fatal record, wait for the dispatch thread and the delivery
lock only while `Delivery::progress()` moves; after the shutdown timeout
without progress they give up (`StallWatch` in `src/core.cpp`). An abandoned
Core reports its queued count, leaves its sinks and its thread alone, and a
Core object that owned it moves its `Impl` to a never-destroyed list.

fork(). `src/fork_handlers.cpp` registers one set of `pthread_atfork`
handlers. Before the fork they take every short-held lock of every Core,
line writer, dashboard and the process context (never the delivery lock,
which a callback may hold while it waits for the forking thread); in the
child they discard the queued records (the parent delivers them),
re-initialise the delivery lock and the condition variables, clear the
sinks' busy flags and forget the threads, which are started again at the
first use. Python adds `os.register_at_fork` hooks for its own state and, in
a `multiprocessing` child, runs its exit hook as a `multiprocessing`
finalizer.

After a fatal record the process ends with `std::abort()`, unless the
program installed a terminate handler (`src/termination.hpp`): the C++
library's default handler writes to standard error first, which never
returns when standard error is blocked.

The Python module has to detach its sinks earlier, while the interpreter is
still running: an `atexit` function (`python/interpreter_exit.cpp`) delivers
what is queued (to Python sinks for at most one second, see
[Python](python.md#exit)), removes the Python sinks, restores the default
diagnostic handler and closes a gate that every later call into Python checks.
