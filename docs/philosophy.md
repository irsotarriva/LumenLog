# Philosophy

## The problem with traditional logging

In a typical logging library, every call site decides everything:

```cpp
// Traditional logging — the producer carries the consumer's burden
logger.log(INFO, "event", "energy={:.2f}", e,
           "subsystem=calorimeter", "run_id=42", "event_id=4821",
           output_to="calorimeter.log", format="key=value");
```

This couples the producer to every consumer. Adding a new output format means touching every log line. Tuning verbosity requires manual changes across the codebase. Adding context to a class's logs means modifying every method in the class.

## The Lumen approach: produce and forget

Lumen separates two concerns that are tangled together in every other logging system:

| Who | Responsibility |
|---|---|
| **Producer** | Write semantic content only — the message and, optionally, explicit tags |
| **Lumen** | Assemble context from ambient sources, route to consumers |
| **Consumer (sink)** | Decide format, color, destination, and which records to receive |

The producer never decides where a record goes or how it looks. The consumer never needs the producer to add annotations it didn't think to include at development time.

```cpp
// Lumen: the producer writes only what it uniquely knows
LOG_INFO("cluster energy {:.2f} GeV", energy)
    .tag("subsystem", "calorimeter");
```

The record that reaches the consumer carries far more than the producer wrote: the class name, the function, the file and line, process-wide tags like job ID and hostname, thread identity, scope-level tags like the current run and event — all merged automatically, none written by the producer.

## Context layers: the memory of the system

Context doesn't need to be repeated. If a class knows it's a calorimeter, it says so once. If a loop knows it's processing event 4821, it says so once. Lumen remembers.

### The five layers (highest priority first)

```
explicit .tag()                          ← "I am overriding everything"
    │
instance tags (lumen::Tagged)            ← "I am a JetFinder, algorithm=antikt"
    │
scope stack (LUMEN_SCOPE, LUMEN_LOOP)    ← "We are processing run 42"
    │
thread tags (set_thread_tag)             ← "I am worker thread 3"
    │
process tags (set_process_tag)           ← "I am job SLURM-8821 on host beam05"
```

When the same key appears at multiple layers, the higher layer wins. This preserves producer control: an explicit `.tag("run_id", "99")` always overrides any ambient run_id, without changing global state.

### Why this matters

A physicist writing an event loop tags the run once. Every `LOG_DEBUG` call anywhere in the codebase — including calls in third-party libraries — automatically carries the run identifier. When they re-run with a new dataset, they change one tag, not thousands of log lines.

A game developer tags the frame once. Every log from physics, rendering, and audio subsystems carries frame number and delta time. When a spike shows up in a metric, they can filter by frame to see exactly what happened.

An ML engineer inherits from `lumen::Tagged` in their model class. Every log from every method carries the model name, epoch, and batch size — without a single `.tag()` call inside `forward()`.

## Producers don't allocate

The emit path does the minimum needed to make a record safe to consume on another thread. While a record is being built, its message and tags are copied into a 512-byte buffer on the builder's stack (longer strings spill to the heap). When the statement ends, everything is packed into one immutable, reference-counted block owned by the record: one heap allocation per record, and no locks. Records never point into the producer's memory, so logging temporaries is safe and a record stays valid for as long as a sink keeps a copy.

## Consumers don't slow producers

Producers push records into lock-free MPSC ring buffers via a single CAS (compare-and-swap) on the buffer head. No mutex, no system call, no blocking. The dispatch thread is the sole consumer, so it can drain without synchronization.

Sinks run on the dispatch thread but must be non-blocking. I/O work (file writes, network sends, rendering) is deferred to sink-owned threads. The `on_log` / `on_metric` / `on_progress` methods should enqueue and return, never block.

## Release builds are zero-cost

Below the threshold set by `LUMEN_MIN_LEVEL`, every log macro expands to `((void)0)`. The compiler never sees the format string, never builds the tag set, never allocates. This is not silent output — this is no output, no execution, no presence in the binary.

Metrics and progress records are always emitted regardless of `LUMEN_MIN_LEVEL`. They carry no debug/trace semantics and are intended for live monitoring in production.

## Extensibility without modification

Adding a new sink requires no changes to core library files. Sinks implement a virtual interface and register with the core singleton. Bridges (framework hooks) are standalone headers that depend only on the producer API — include one and framework calls emit records automatically, with zero changes to the framework or to user code.
