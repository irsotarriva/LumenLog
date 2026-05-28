# Lumen — Public API Contract

This document defines the interface a user of Lumen sees. It is the contract between the library and its consumers. Implementation details not visible through these headers are not part of this contract.

---

## Headers

```
lumen/lumen.h          — full include (core + macros + helpers)
lumen/core.h           — Core singleton, sink registration, process tags
lumen/record.h         — LogRecord, MetricRecord, ProgressRecord, TagSet
lumen/sink.h           — Sink base class, built-in sinks
lumen/predicate.h      — Predicate DSL
lumen/tagged.h         — lumen::Tagged mixin
lumen/bridges/*.h      — optional framework bridges (each independent)
```

A user who only wants to emit records needs only `lumen/lumen.h`.

---

## Log Macros

```cpp
LOG_TRACE(fmt, ...)
LOG_DEBUG(fmt, ...)
LOG_INFO (fmt, ...)
LOG_WARN (fmt, ...)
LOG_ERROR(fmt, ...)
LOG_FATAL(fmt, ...)
```

`fmt` follows `std::format` conventions. The macros return a `RecordBuilder&` so `.tag()` may be chained. The record is committed when the `RecordBuilder` temporary is destroyed at the end of the full expression.

```cpp
// No tags — simplest form
LOG_INFO("cluster energy {:.2f} GeV", energy);

// With explicit tags
LOG_WARN("energy out of range, got {:.2f}", e)
    .tag("subsystem", "calorimeter")
    .tag("threshold", "50.0");

// FATAL also calls std::terminate after dispatch completes
LOG_FATAL("unrecoverable state in event {}", event_id);
```

Below the compile-time threshold set by `LUMEN_MIN_LEVEL`, all macro calls expand to `((void)0)` and generate no code.

---

## Metric Emission

```cpp
// Emit a named scalar value
lumen::metric("loss", loss_value);
lumen::metric("gpu_mem_mb", mem).tag("device", "0");
lumen::metric("fps", frame_rate);
```

`lumen::metric()` returns a `MetricBuilder&` supporting the same `.tag()` chaining. Metric records are never elided by `LUMEN_MIN_LEVEL`.

---

## Progress Emission

```cpp
// Create a progress tracker (returns a ProgressHandle)
auto job = lumen::progress("jet clustering", total_events);

// Update — call inside the loop
job.update(current);          // absolute position
job.tick();                   // increment by 1
job.finish();                 // mark complete (optional — also triggered when handle destructs)
```

A `ProgressHandle` emits a `ProgressRecord` on each `update()` / `tick()` call. When the handle is destroyed or `finish()` is called, a final record with `current == total` is emitted, signaling the consumer to retire the bar.

```cpp
// Convenience: LUMEN_LOOP wraps a range with a progress handle automatically
LUMEN_LOOP("event loop", i, events.size()) {
    process(events[i]);
    // all LOG_* calls here carry: loop_label="event loop", iteration=i, total=events.size()
}
```

---

## Scope Helpers

```cpp
// Push arbitrary tags for the current lexical scope
{
    LUMEN_SCOPE("run_id", run_id, "dataset", "mc23");
    process_run();   // all records here carry run_id and dataset
}   // tags removed here

// Frame scope — carries frame_number and delta_time_ms
LUMEN_FRAME_SCOPE(frame_counter, delta_ms) {
    update(delta_ms);
    render();
}

// Activate this instance's tags for the duration of this method call
void MyClass::process() {
    LUMEN_MEMBER_SCOPE;   // attaches tags registered via lumen_tag() in constructor
    LOG_INFO("processing");
}
```

All scope helpers are RAII — safe under exceptions and early returns.

---

## Instance Tags — lumen::Tagged

```cpp
class JetFinder : public lumen::Tagged {
public:
    JetFinder(int run_id, std::string algo) {
        lumen_tag("class",     "JetFinder");
        lumen_tag("run_id",    std::to_string(run_id));
        lumen_tag("algorithm", std::move(algo));
    }

    void cluster(const EventData& ev) {
        LUMEN_MEMBER_SCOPE;    // activates the tags above for this call
        LOG_DEBUG("starting cluster step");
        // record carries: class=JetFinder, run_id=42, algorithm=antikt
        //                 + call site: function=cluster, file=..., line=...
    }
};
```

`lumen::Tagged` adds a `TagSet<8>` member and the `lumen_tag()` helper. It has no virtual functions and no other data members. Inheriting from it adds 8 × (2 × sizeof(string\_view)) + 1 byte to the object's size.

---

## Thread and Process Tags

```cpp
// Set once at process startup — attached to every record from this process
lumen::set_process_tag("job_id",  slurm_job_id());
lumen::set_process_tag("host",    hostname());
lumen::set_process_tag("rank",    std::to_string(mpi_rank));

// Set once per thread at thread creation
lumen::set_thread_tag("thread_name", "worker-3");
lumen::set_thread_tag("cpu",         std::to_string(cpu_id));
```

---

## Core — Sink Registration

```cpp
#include "lumen/core.h"
#include "lumen/sink.h"
#include "lumen/predicate.h"

auto& core = lumen::core();

// Register a terminal sink that receives INFO and above from any source
core.add_sink(
    std::make_unique<lumen::TerminalSink>(),
    lumen::level_at_least(lumen::INFO)
);

// Register a file sink that receives everything from the calorimeter subsystem
core.add_sink(
    std::make_unique<lumen::FileSink>("calorimeter.log"),
    lumen::tag_equals("subsystem", "calorimeter")
);

// Register a JSON sink for all metrics regardless of source
core.add_sink(
    std::make_unique<lumen::JsonSink>("metrics.jsonl"),
    lumen::always()
);

// Remove a sink (returns the unique_ptr)
auto sink_ptr = core.remove_sink(sink_id);

// Flush all sinks (blocks until all pending records are written)
core.flush();
```

`add_sink` returns a `SinkId` (an opaque integer) used for later removal.

---

## Predicate DSL

```cpp
using namespace lumen;

// Primitives
always()
never()
level_at_least(LogLevel::WARN)
level_equals(LogLevel::ERROR)
tag_equals("subsystem", "tracking")
tag_exists("event_id")

// Composition — operator overloads on Predicate
level_at_least(WARN) && tag_equals("subsystem", "calorimeter")
tag_equals("run_id", "42") || tag_equals("run_id", "43")
!tag_exists("suppress")

// Metrics and progress are separate record types;
// a LogRecord predicate does not filter MetricRecords.
// Each sink declares which record types it handles via on_log / on_metric / on_progress.
```

Predicates are value types. They are composed with `&&`, `||`, `!` and copied into the sink at registration time. They are immutable after that.

---

## Sink Customization

### TerminalSink

```cpp
lumen::TerminalSink::Config cfg;

// Level colors — ANSI fg color code
cfg.colors[lumen::DEBUG] = lumen::AnsiColor::Cyan;
cfg.colors[lumen::INFO]  = lumen::AnsiColor::White;
cfg.colors[lumen::WARN]  = lumen::AnsiColor::Yellow;
cfg.colors[lumen::ERROR] = lumen::AnsiColor::Red;
cfg.colors[lumen::FATAL] = lumen::AnsiColor{ .fg=196, .bold=true };  // bright red bold

// Timestamp format (strftime-style)
cfg.time_format = "%H:%M:%S.%ms";

// Which tags to show inline in the log line (others available via structured output)
cfg.inline_tags = { "subsystem", "event_id" };

// Dashboard: which metrics to display as sparklines
cfg.dashboard_metrics = { "loss", "gpu_mem_mb", "fps" };
cfg.dashboard_height   = 5;  // lines reserved at top of terminal

auto sink = std::make_unique<lumen::TerminalSink>(std::move(cfg));
```

### FileSink

```cpp
lumen::FileSink::Config cfg;
cfg.path         = "run_{date}.log";   // {date} substituted at open time
cfg.rotate_mb    = 256;                // rotate after 256 MB
cfg.max_files    = 5;                  // keep last 5 rotated files
cfg.format       = lumen::FileFormat::KeyValue;  // or PlainText, Json

auto sink = std::make_unique<lumen::FileSink>(std::move(cfg));
```

### Custom sink

```cpp
class MySink : public lumen::Sink {
public:
    void on_log(const lumen::LogRecord& r) override {
        // r.message, r.level, r.tags, r.timestamp_ns, r.source all available
        // Must be non-blocking — enqueue for async I/O if needed
    }
    void on_metric(const lumen::MetricRecord& r) override { ... }
    void on_progress(const lumen::ProgressRecord& r) override { ... }
    void flush() override { ... }
};

core.add_sink(std::make_unique<MySink>(), lumen::always());
```

---

## RecordBuilder — tag chaining reference

```cpp
// RecordBuilder is returned by LOG_* macros and lumen::metric()
// It is a temporary — do not store it

struct RecordBuilder {
    RecordBuilder& tag(std::string_view key, std::string_view value);
    RecordBuilder& tag(std::string_view key, int64_t value);      // formatted to string
    RecordBuilder& tag(std::string_view key, double value);       // formatted to string
    // destructor commits the record to the ring buffer
};
```

---

## Bridge Opt-in

```cpp
// Each bridge is a standalone header — no other changes required
#include "lumen/lumen.h"
#include "lumen/bridges/torch.h"     // auto-logs forward/backward, loss, lr

// From here, torch::nn::Module subclasses automatically emit MetricRecords
// The user's LOG_* calls are unaffected
auto model = MyModel();
model->train();   // MetricRecord: "forward_ms", "loss", "grad_norm" emitted automatically
```

Bridge headers must be included after the framework header they wrap. They produce no code if the framework header is not present (guarded with `#ifdef`).

---

## Python API (when built with LUMEN_ENABLE_PYTHON)

```python
import lumen

# Same three primitives
lumen.info("cluster energy {:.2f} GeV".format(energy))
lumen.warn("threshold exceeded").tag("subsystem", "calorimeter")

lumen.metric("loss", loss_value).tag("epoch", str(epoch))

bar = lumen.progress("training", total_batches)
bar.tick()

# Process tags
lumen.set_process_tag("experiment", experiment_name)

# Sink registration mirrors C++ API
lumen.core().add_sink(lumen.TerminalSink(), lumen.level_at_least("INFO"))
```

Python records flow into the same broker and are dispatched by the same dispatch thread. A Python-side `logging.Handler` subclass is also provided so the standard `logging` module forwards into Lumen automatically when imported.

---

## Invariants and Guarantees

- `LOG_*` macros are safe to call from any thread at any time after `lumen::core()` is first accessed.
- Emit calls never throw. On arena overflow, the record is truncated and a single `WARN` record is emitted to stderr; the original record is still delivered with a truncation marker tag.
- `LOG_FATAL` flushes all sinks before calling `std::terminate`. No records are lost.
- Records are delivered to sinks in emission order per producer thread. Cross-thread ordering is not guaranteed — this matches standard MPSC semantics.
- A sink's `on_*` methods are always called from the dispatch thread, never from a producer thread.
- Predicates registered with a sink are evaluated only on the dispatch thread and are never called concurrently.
- `lumen::Tagged` is safe to use with multiple inheritance. It carries no virtual functions.
