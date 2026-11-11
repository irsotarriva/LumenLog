# Usage

What a program writes to produce records. Everything here goes to the global
`lumen::core()` unless a section says otherwise. The headers are the
reference: `lumen/macros.hpp` (statements, scopes, loops),
`lumen/record_builder.hpp` (tags, fatal), `lumen/progress.hpp`,
`lumen/context.hpp` and `lumen/tagged.hpp`.

The fragments on this page are compiled by the build as function bodies, with
`#include "lumen/lumen.hpp"` and declarations of the made-up functions they
call (`read_energy`, `process`, ...).

## Log statements

<!-- check: prelude=common wrap=body -->
```cpp
LUMEN_LOG_INFO("plain text, logged verbatim: {braces} are not a format");
LUMEN_LOG_INFO("energy {:.2f} GeV in {} hits", read_energy(), 12);
LUMEN_LOG_WARN("threshold crossed").tag("detector", "calorimeter").tag("channel", 17);
```

- With one argument the text is the message, as it is. With more, the first
  is a `std::format` string, checked at compile time against the arguments.
- `LUMEN_LOG_TRACE`, `LUMEN_LOG_DEBUG`, `LUMEN_LOG_INFO`, `LUMEN_LOG_WARN`
  and `LUMEN_LOG_ERROR` end in a `lumen::RecordBuilder` expression, so
  `.tag()` calls can follow. Each is one statement, safe as the body of an
  `if` without braces, with or without `else`. `LUMEN_LOG_FATAL` is
  described [below](#fatal).
- `LOG_TRACE` … `LOG_FATAL` are the same macros under short names. Define
  `LUMEN_NO_SHORT_MACROS` before including Lumen to keep those names free for
  another library.
- The record is submitted when the statement ends (the builder's destructor).
  That is when it gets its timestamp (`lumen::now_ns()`, nanoseconds since the
  Unix epoch), its thread number (`lumen::this_thread_id()`: 1 for the first
  thread that asks, then 2, ...) and its context tags.
- The source location is recorded from `std::source_location`: file, line,
  function, and the innermost enclosing class. The class is read from the
  function's signature text, so a free function in a namespace gets the
  namespace's name, and a free function at global scope gets `""`.

### Tags

- `.tag(key, value)` copies both strings at once, so temporaries are fine.
- Integers are written exactly, floating-point values in the shortest form
  that reads back as the same `double`, `bool` as `true` or `false`. So
  `.tag("x", 0.1)` stores `0.1`, and a numeric predicate compares with the
  value you logged.
- Tagging a key twice keeps the later value.
- A record holds at most 16 tags (`lumen::max_record_tags`). A `.tag()` with
  a new key on a full record is ignored and counted in
  `Core::statistics().tags_dropped`.

## Compile-time level threshold

The CMake option `LUMEN_MIN_LEVEL` (default `DEBUG`) sets the lowest level
that is compiled. It reaches the headers as the macro `LUMEN_MIN_LEVEL_VALUE`,
the level's number (0 = Trace … 5 = Fatal), so that a consumer's own `DEBUG`
or `ERROR` macro cannot change its meaning. A statement below the threshold is
in the discarded branch of an `if constexpr`: it produces no code and its
arguments are not evaluated. With the default, `LUMEN_LOG_TRACE` statements
are removed. Fatal statements are never removed.

<!-- check: prelude=common wrap=body -->
```cpp
static_assert(lumen::level_compiled_in(lumen::LogLevel::Info));  // with the default threshold
if constexpr (lumen::level_compiled_in(lumen::LogLevel::Trace)) {
    LUMEN_LOG_INFO("built with LUMEN_MIN_LEVEL=TRACE");
}
```

When it is not `DEBUG`, `LUMEN_MIN_LEVEL_VALUE` is defined for every target
that links `lumen::lumen`, so the library and its users agree. Do not
define `LUMEN_MIN_LEVEL` in your own code: `lumen/macros.hpp` stops with an
error, because the CMake option of that name takes a level name, not a number.
There is no run-time level setting: use a [predicate](predicates.md) on each
sink.

## Metrics

<!-- check: prelude=common wrap=body -->
```cpp
lumen::metric("loss", 0.25).tag("epoch", 3);
lumen::metric("temperature_k", 291.4);
```

A metric record has a name, a `double` value, tags, a timestamp and a thread
number. Metrics are never removed at compile time.

## Progress bars

<!-- check: prelude=common wrap=body -->
```cpp
const std::vector<std::string> files = list_files();
auto bar = lumen::progress("files", files.size());
for (const std::string& file : files) {
    copy_file(file);
    bar.tick();
}
bar.finish();
```

`current` counts completed units of work. Every record the handle sends is a
state that was reached:

- Creating the handle sends nothing.
- `update(n)` sends `n/total`; `tick()` sends `current + 1`. A call that would
  send the state sent last sends nothing.
- `finish()` sends `total/total` once, if that was not the last state sent,
  and ends the bar: later calls do nothing.
- Destroying an unfinished handle sends nothing. The last record is the last
  progress made; a sink that needs to know that a bar was abandoned sees that
  `total/total` never came.
- A moved-from handle does nothing.

Every record of one bar has the same `id`, unique in the program. A handle is
not synchronised: use it from one thread at a time.

## Loops

<!-- check: prelude=common wrap=body -->
```cpp
const uint32_t n_events = 1000;
LUMEN_LOOP("event", event, n_events) {
    process(event);   // records logged here carry event=<event>
}
```

`LUMEN_LOOP(label, var, total)` is a `for` loop over `var = 0 … total - 1`,
with `var` const and of the type of `total` (any integer type). While a body
runs, records carry the scope tag `<label>=<var>`. The loop also drives a
progress bar named `label` that counts completed bodies: `0/n` before the
first body, `k/n` after the k-th, `n/n` after the last. With `total` 0 (or
negative) it sends the single record `0/0`. Leaving early (`break`, `return`,
an exception) sends nothing more. `break` and `continue` refer to the loop
itself. The label and the total are evaluated once. The macro needs `var` to
be a plain name, and no argument may contain a comma outside parentheses.

## Context

Tags that a record gets without the statement naming them. When a record is
submitted, Core keeps its explicit tags and then adds each layer in this
order, skipping keys the record already has, until it holds 16 tags:

1. explicit: `.tag(k, v)` on the statement;
2. instance: the `lumen::Tagged` objects whose `LUMEN_MEMBER_SCOPE` is active
   on this thread, innermost first;
3. scope: `LUMEN_SCOPE` and `LUMEN_LOOP` active on this thread, innermost
   first;
4. thread: `lumen::set_thread_tag` on this thread;
5. process: `lumen::set_process_tag`, from any thread.

The first layer to name a key wins. When the record is full, the lowest
layers lose, and every tag left out is counted in `tags_dropped`. Within one
layer a key is held once; setting it again replaces the value. Context belongs
to the calling thread (the process layer to the program), not to a Core: every
Core reads the same context.

This program shows each layer (`examples/context_layers.cpp`, run by ctest as
`Example.ContextLayers`):

<!-- check: from=examples/context_layers.cpp -->
```cpp
// Context layers: which tags a record carries without the log statement
// naming them. Each line on standard error shows the tags app, worker,
// detector, run and event, in that order, for the keys the record has.
//
// Priority, highest first: explicit .tag(), the Tagged object of the
// innermost LUMEN_MEMBER_SCOPE, LUMEN_SCOPE / LUMEN_LOOP (innermost first),
// the thread's tags, the process's tags. The first layer to name a key wins.
#include <cstdint>
#include <expected>
#include <iostream>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include "lumen/lumen.hpp"

namespace {

// An object whose own tags are carried by the records logged in its member
// scopes.
class Detector : public lumen::Tagged {
public:
    // A factory, because tagging can fail and a constructor cannot report it.
    [[nodiscard]] static std::expected<Detector, std::error_code> create(std::string_view name) {
        Detector detector;
        if (auto tagged = detector.lumen_tag("detector", name); !tagged) {
            return std::unexpected(tagged.error());
        }
        return detector;
    }

    void calibrate(uint32_t channels) const {
        LUMEN_MEMBER_SCOPE;
        LUMEN_LOG_INFO("calibrating {} channels", channels);
    }

private:
    Detector() = default;
};

// Runs on its own thread, so that its thread tag is its own.
[[nodiscard]] std::expected<void, std::error_code> run_worker(const Detector& detector) {
    if (auto set = lumen::set_thread_tag("worker", "1"); !set) {
        return std::unexpected(set.error());
    }
    LUMEN_LOG_INFO("worker started");

    LUMEN_SCOPE("run", "7");
    LUMEN_SCOPE("detector", "none");
    LUMEN_LOG_INFO("inside two scopes");
    detector.calibrate(128);  // the instance tag detector=tracker beats the scope's
    LUMEN_LOG_INFO("explicit tag").tag("run", "8");

    LUMEN_LOOP("event", event, uint32_t{2}) {
        LUMEN_LOG_DEBUG("processing event {}", event);
    }
    return {};
}

}  // namespace

int main() {
    lumen::TerminalSink::Config config;
    config.time_format = "";  // no time column
    config.inline_tags = {"app", "worker", "detector", "run", "event"};
    auto terminal = lumen::TerminalSink::create(std::move(config));
    if (!terminal) {
        std::cerr << "cannot create the terminal sink: " << terminal.error().message() << '\n';
        return 1;
    }
    if (const auto id = lumen::core().add_sink(std::move(*terminal)); !id) {
        std::cerr << "cannot register the terminal sink: " << id.error().message() << '\n';
        return 1;
    }
    if (const auto set = lumen::set_process_tag("app", "context_layers"); !set) {
        std::cerr << "cannot set the process tag: " << set.error().message() << '\n';
        return 1;
    }

    const auto detector = Detector::create("tracker");
    if (!detector) {
        std::cerr << "cannot tag the detector: " << detector.error().message() << '\n';
        return 1;
    }

    std::expected<void, std::error_code> worker_result;
    // std::thread rather than std::jthread: Apple's libc++ lacks jthread.
    std::thread worker([&] { worker_result = run_worker(*detector); });
    worker.join();
    if (!worker_result) {
        std::cerr << "worker failed: " << worker_result.error().message() << '\n';
        return 1;
    }

    LUMEN_LOG_INFO("main thread: no worker tag, no scope");

    lumen::core().flush();
    return 0;
}
```

Its output:

<!-- check: output=examples/expected/context_layers.stderr -->
```text
INFO  worker started app=context_layers worker=1
INFO  inside two scopes app=context_layers worker=1 detector=none run=7
INFO  calibrating 128 channels app=context_layers worker=1 detector=tracker run=7
INFO  explicit tag app=context_layers worker=1 detector=none run=8
PROGRESS event 0/2 app=context_layers worker=1 detector=none run=7 event=
DEBUG processing event 0 app=context_layers worker=1 detector=none run=7 event=0
PROGRESS event 1/2 app=context_layers worker=1 detector=none run=7 event=0
DEBUG processing event 1 app=context_layers worker=1 detector=none run=7 event=1
PROGRESS event 2/2 app=context_layers worker=1 detector=none run=7 event=1
INFO  main thread: no worker tag, no scope app=context_layers
```

The loop's progress records carry the loop's own scope tag with the index of
the last body that ran (empty before the first).

### Process and thread tags

<!-- check: prelude=common wrap=body -->
```cpp
if (const auto set = lumen::set_process_tag("host", "node07"); !set) {
    LUMEN_LOG_WARN("host tag not set: {}", set.error().message());
}
const bool removed = lumen::remove_thread_tag("worker");  // false if it was not set
LUMEN_LOG_DEBUG("worker tag removed: {}", removed);
lumen::clear_process_tags();
```

`set_process_tag` and `set_thread_tag` copy key and value and fail with
`LumenError::TagTableFull` when the key is new and the table is full (16
process keys, 8 per thread). A thread tag is seen only by records of the
thread that set it, and is destroyed when the thread exits. The process table
is never destroyed, so logging during static destruction still reads it. The
process table is guarded by a mutex; each thread reads a copy of it that is
refreshed only after a change.

### Scopes

`LUMEN_SCOPE(key, value)` adds `key=value` to the records of the calling
thread until the end of the enclosing block. It copies both strings. Scopes
have no limit of their own; only the 16 tags of a record bound how many of
them reach it.

### Tagged objects

A class derived from `lumen::Tagged` has tags of its own (at most 8 keys,
`lumen::max_instance_tags`), set with `lumen_tag(key, value)` and removed with
`lumen_untag(key)`. `LUMEN_MEMBER_SCOPE;` in a member function makes them part
of the thread's context until the end of the block. The tags are read when
each record is submitted, so a `lumen_tag()` inside the scope applies to the
records after it. A copy of the object gets a copy of the tags; a moved-from
object has none. `sizeof(lumen::Tagged)` is one pointer, and an object that
was never tagged owns nothing else.

A `Tagged` object is not synchronised: tagging it, copying it or moving it
must not happen while another thread is inside one of its member scopes.
Several threads may be inside member scopes of the same object at once.

### Rules for every scope

`LUMEN_SCOPE`, `LUMEN_LOOP` and `LUMEN_MEMBER_SCOPE` create local guard
objects. A guard must be destroyed on the thread that created it, in reverse
order of creation, which is what block scope does. Moving a guard's lifetime
to another thread (a coroutine resumed elsewhere, a task handed to a pool) is
not supported; debug builds assert on it. The object of a
`LUMEN_MEMBER_SCOPE` must outlive the scope.

## Fatal

`LUMEN_LOG_FATAL("...", args...)` is a statement, not an expression: it takes
no `.tag()`. It delivers, on the calling thread, every record queued before
it, then the fatal record, flushes every sink and calls `std::terminate()`.
Inside a sink callback of the same Core, the sink whose callback is running is
skipped; every other sink receives the record and is flushed. After the Core
has shut down the record is only counted, and the program still terminates.

## Other Cores

`lumen::Core` can also be constructed directly, for instance in a test. The
macros, `lumen::metric` and `lumen::progress` always use `lumen::core()`; for
another Core, use the builders and the progress handle with that Core:

<!-- check: prelude=common wrap=body -->
```cpp
lumen::Core local;
lumen::RecordBuilder(local, lumen::LogLevel::Info, std::source_location::current(), "to local");
lumen::MetricBuilder(local, "loss", 0.25).tag("epoch", 3);
lumen::ProgressHandle bar(local, "steps", 10);
bar.tick();
local.flush();
```

A Core shuts down in its destructor like the global one. Its producers hold a
reference to it and must not outlive it. Context tags are shared by all Cores.

## Counters and diagnostics

`Core::statistics()` returns, for each record type, the records accepted, the
records refused because the queue was full (`dropped_queue_full`) and those
submitted after shutdown (`dropped_after_shutdown`), plus `tags_dropped`. The
values are exact when read while nothing is being submitted.

Problems that cannot be returned to a caller (the first record refused by a
full queue, the first tag left out of a full record, the first exception of
each sink) are reported once each, as one line of text, to the Core's
diagnostic handler. The default writes the line to standard error;
`set_diagnostic_handler` replaces it, and an empty handler discards the lines.

<!-- check: prelude=common wrap=body -->
```cpp
const lumen::CoreStatistics stats = lumen::core().statistics();
if (stats.logs.dropped_queue_full > 0) {
    LUMEN_LOG_WARN("{} log records were lost to a full queue", stats.logs.dropped_queue_full);
}
lumen::core().set_diagnostic_handler([](std::string_view line) { std::clog << line << '\n'; });
```
