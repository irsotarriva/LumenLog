# Usage Guide

## Logging

Six severity levels, each with a corresponding macro:

```cpp
LOG_TRACE("detailed instrumentation: value={}", x);
LOG_DEBUG("cluster size after cut: {}", n);
LOG_INFO("starting event loop for run {}", run_id);
LOG_WARN("energy out of range, got {:.2f}", e);
LOG_ERROR("failed to open geometry file: {}", path);
LOG_FATAL("unrecoverable state: {}", details);   // calls std::terminate after dispatch
```

`LOG_FATAL` flushes all sinks before terminating — no records are lost.

Macros use `std::format` conventions for format strings. Arguments are type-safe.

### Tag chaining

Every log macro returns a `RecordBuilder&` that supports fluent `.tag()` calls:

```cpp
LOG_INFO("cluster energy {:.2f}", energy)
    .tag("subsystem", "calorimeter")
    .tag("event_id", "4821");
```

Tag values can be strings or numeric:

```cpp
LOG_INFO("processed").tag("count", 42);         // integer
LOG_INFO("loss").tag("value", 0.0314);          // double
```

The record is committed when the `RecordBuilder` temporary is destroyed at the end of the full expression. Do not store the builder reference.

### Compile-time elision

Set `LUMEN_MIN_LEVEL` via CMake:

```sh
cmake -DLUMEN_MIN_LEVEL=INFO ...
```

Calls below the threshold (`DEBUG` and `TRACE` in this case) compile to nothing — no formatting, no allocation, no function call.

## Metrics

Emit a named scalar value:

```cpp
lumen::metric("loss", loss_value);
lumen::metric("gpu_mem_mb", current_mem).tag("device", "0");
lumen::metric("fps", 1.0f / delta);
```

`lumen::metric()` returns a `MetricBuilder&` with the same `.tag()` chaining. Metric records are never elided by `LUMEN_MIN_LEVEL`.

## Progress

Track completion of a named job:

```cpp
auto job = lumen::progress("jet clustering", total_events);

for (uint64_t i = 0; i < total_events; i++) {
    process(events[i]);
    job.tick();          // increment by 1 and emit update
}
// ProgressHandle destructor emits final record with current == total
```

Progress handle methods:

```cpp
job.update(position);    // set absolute position
job.tick();              // increment by 1
job.finish();            // mark complete (optional — destructor auto-finishes)
```

Each update emits a `ProgressRecord`. Consumers (typically the terminal dashboard) render a progress bar that fills as `current` approaches `total`.

## Scope helpers

### LUMEN_SCOPE

Push tags for the current lexical block:

```cpp
void process_run(int run_id) {
    LUMEN_SCOPE("run_id", std::to_string(run_id));
    // All log/metric/progress calls in this scope carry run_id
    LOG_INFO("run started");
    process_events();
    // Tags removed when scope exits
}
```

Multiple key-value pairs:

```cpp
LUMEN_SCOPE("run_id", "42", "dataset", "mc23", "phase", "reco");
```

Nested scopes merge — innermost tags override outer tags for the same key.

### LUMEN_LOOP

Progress bar with automatic scope tags:

```cpp
LUMEN_LOOP("training batches", batch, num_batches) {
    forward_pass(batch);
    // All records carry: loop_label="training batches", iteration=batch, total=num_batches
    // Progress bar auto-updates each iteration
}
```

`LUMEN_LOOP` creates a `ProgressHandle`, pushes scope tags, and ticks the progress bar at each iteration start. On scope exit, it pops the tags and finishes the progress bar.

### LUMEN_FRAME_SCOPE

Frame-aware scope for game loops:

```cpp
void frame_loop() {
    uint64_t frame = 0;
    while (running) {
        float dt = clock.tick();
        LUMEN_FRAME_SCOPE(frame, dt) {
            update(dt);
            render();
        }
        frame++;
    }
}
```

Carries `frame_number` and `delta_time_ms` on every record inside the frame.

### LUMEN_MEMBER_SCOPE

Activate instance tags for the duration of a method:

```cpp
class JetFinder : public lumen::Tagged {
public:
    JetFinder() {
        lumen_tag("class", "JetFinder");
        lumen_tag("algorithm", "antikt");
    }

    void cluster() {
        LUMEN_MEMBER_SCOPE;
        LOG_INFO("clustering");
        // Record carries: class=JetFinder, algorithm=antikt
    }
};
```

Without `LUMEN_MEMBER_SCOPE`, instance tags are not active. This is deliberate — you control which methods carry instance context.

All scope helpers are RAII. Exceptions and early returns are safe.

## Instance tags (lumen::Tagged)

Inherit from `lumen::Tagged` to give your class its own tag storage:

```cpp
class MyProcessor : public lumen::Tagged {
public:
    MyProcessor(int id, std::string name) {
        lumen_tag("processor_id", std::to_string(id));
        lumen_tag("processor_name", std::move(name));
    }
};
```

`lumen::Tagged` stores up to 8 tags. It has no virtual functions — the overhead is 8 pairs of `string_view` plus a count byte.

Use `LUMEN_MEMBER_SCOPE` at the top of methods where you want instance tags active.

## Thread and process tags

Set once, attached to every record automatically:

```cpp
// Process tags — set at startup
lumen::core().set_process_tag("job_id", "SLURM-8821");
lumen::core().set_process_tag("host", hostname());
lumen::core().set_process_tag("rank", std::to_string(mpi_rank));

// Thread tags — set at thread creation
lumen::core().set_thread_tag("worker", "analysis-3");
lumen::core().set_thread_tag("cpu", std::to_string(cpu_id));
```

Process tags apply to all threads in the process. Thread tags apply to all records emitted from that thread.

## Sink registration

Register sinks before emitting records:

```cpp
auto& core = lumen::core();

// Terminal sink — colorized output to stderr
core.add_sink(
    std::make_unique<lumen::TerminalSink>(),
    lumen::level_at_least(lumen::INFO)
);

// File sink — plain text log
core.add_sink(
    std::make_unique<lumen::FileSink>("run.log"),
    lumen::always()
);

// JSON sink — structured output
core.add_sink(
    std::make_unique<lumen::JsonSink>("run.jsonl"),
    lumen::always()
);
```

`add_sink` returns a `SinkId` for later removal with `core.remove_sink(id)`.

Call `core.flush()` before process exit to ensure all records are written.

## Python usage (when LUMEN_ENABLE_PYTHON=ON)

```python
import lumen_bindings as lumen

lumen.info("analysis starting").tag("phase", "init")
lumen.metric("loss", 0.0314)
lumen.set_process_tag("experiment", "my_experiment")

bar = lumen.progress("training", 1000)
for i in range(1000):
    bar.tick()

lumen.core().add_sink(lumen.TerminalSink(), lumen.level_at_least("INFO"))
```
