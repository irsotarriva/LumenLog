# Lumen — Architecture Document

## System Overview

```
┌──────────────────────────────────────────────────────────────────┐
│  PRODUCER LAYER                                                  │
│  LOG_INFO / LOG_WARN / logger.metric() / logger.progress()       │
│  Macros expand at call site — source_location captured here      │
└───────────────────────────┬──────────────────────────────────────┘
                            │  emit (lock-free, producer never blocks)
┌───────────────────────────▼──────────────────────────────────────┐
│  CONTEXT ASSEMBLY                                                │
│  Merges: explicit tags → instance tags → scope stack →           │
│          thread tags → process tags                              │
│  Allocates record into thread-local arena                        │
└───────────────────────────┬──────────────────────────────────────┘
                            │
┌───────────────────────────▼──────────────────────────────────────┐
│  MPSC RING BUFFERS  (one per record type)                        │
│  LogBuffer / MetricBuffer / ProgressBuffer                       │
│  Configurable capacity, configurable overflow policy             │
└───────────────────────────┬──────────────────────────────────────┘
                            │  drained by dispatch thread
┌───────────────────────────▼──────────────────────────────────────┐
│  DISPATCH THREAD                                                 │
│  Evaluates each record against registered sink predicates        │
│  Routes matching records to sink queues                          │
└──────┬──────────────────────────┬───────────────────────────────┘
       │                          │
┌──────▼──────┐            ┌──────▼──────┐            ┌───────────┐
│  SINK A     │            │  SINK B     │            │  SINK N   │
│  predicate  │            │  predicate  │            │  ...      │
│  formatter  │            │  formatter  │            │           │
│  terminal   │            │  JSON file  │            │  network  │
└─────────────┘            └─────────────┘            └───────────┘
```

---

## Record Layer

### TagSet

The tag set is the universal context carrier. It is a fixed-capacity array of string\_view pairs, allocated on the stack up to a compile-time limit N (default 16). No heap allocation occurs unless the record exceeds N tags, in which case overflow tags spill into a per-record arena.

```
TagSet<16>
  ┌──────────────┬───────────────┐
  │ key          │ value         │
  ├──────────────┼───────────────┤
  │ "subsystem"  │ "calorimeter" │
  │ "run"        │ "42"          │
  │ "event_id"   │ "4821"        │
  │ ...          │ ...           │
  └──────────────┴───────────────┘
  count: 3 / capacity: 16
```

Tag keys and values are `std::string_view`. They point into one of:
- Caller stack (for string literals — zero copy, zero allocation)
- Thread-local arena (for dynamically formatted values)
- Instance tag storage (for instance-lifetime tags)

The arena is reset per-record after dispatch completes, not per-emit, so the dispatch thread can safely read all values before the arena is cleared.

### LogRecord

```
LogRecord
  level           : LogLevel  (TRACE=0, DEBUG=1, INFO=2, WARN=3, ERROR=4, FATAL=5)
  message         : string_view  → thread-local arena
  tags            : TagSet<16>
  timestamp_ns    : uint64_t   (nanoseconds since epoch, clock_gettime CLOCK_REALTIME)
  thread_id       : uint32_t
  source          : SourceLocation { file, function, class_name, line }
```

`class_name` is populated from C++26 reflection when available, empty otherwise.

### MetricRecord

```
MetricRecord
  name            : string_view  → static storage (metric names are always literals)
  value           : double
  tags            : TagSet<16>
  timestamp_ns    : uint64_t
```

### ProgressRecord

```
ProgressRecord
  id              : uint64_t    (stable across updates; hash of label + thread_id)
  label           : string_view → thread-local arena or static storage
  current         : uint64_t
  total           : uint64_t
  tags            : TagSet<16>
  timestamp_ns    : uint64_t
```

A ProgressRecord with `current == total` signals completion and the consumer should retire the bar.

---

## Context Assembly

Context assembly runs on the producer thread, synchronously before the record is pushed to the ring buffer. It is the hottest path in the library and must be branchless and allocation-free for the common case.

### Tag merge algorithm

```
merged_tags = {}
for layer in [explicit, instance, scope_stack (innermost first), thread, process]:
    for (k, v) in layer:
        if k not in merged_tags:
            merged_tags[k] = v
```

The first occurrence of a key wins. This is implemented as a linear scan over a small fixed array — at 16 tags this is faster than a hash map due to cache locality.

### Scope stack

The scope stack is a thread-local linked list of `ScopeFrame` objects. Each frame holds a pointer to a TagSet and a pointer to the next frame. Pushing is a pointer assignment; popping restores the previous pointer. No heap allocation.

```
thread_local ScopeFrame* scope_top = nullptr;

struct ScopeFrame {
    const TagSet<>* tags;
    ScopeFrame*     prev;
};
```

RAII helpers (`LUMEN_SCOPE`, `LUMEN_LOOP`) push on construction and pop on destruction, so exceptions and early returns are safe.

### Instance tags

A class inheriting from `lumen::Tagged` stores a `TagSet<8>` as a member. When a member function emits a record, the instance's tag set is included in the merge via a thread-local pointer set on method entry.

The mechanism: `lumen::Tagged` provides a `lumen_scope()` method that returns a RAII guard. The macro `LUMEN_MEMBER_SCOPE` placed at the start of a member function (or injected automatically via a code transform) activates the instance tags for that call's duration. Without the macro the instance tags are not active — this is a deliberate tradeoff favoring zero overhead over implicit magic.

Alternatively, a user may use `LUMEN_CLASS_SCOPE` at the top of each method they want covered, or inherit from `lumen::AutoTagged<Derived>` which uses CRTP to inject the scope guard automatically into all virtual dispatch calls.

---

## MPSC Ring Buffer

One ring buffer per record type. The buffer is a power-of-two array of record slots plus a sequence counter per slot.

```
struct Slot<T> {
    std::atomic<uint64_t> sequence;
    T                     record;
};

template<typename T, size_t N>  // N must be power of two
struct RingBuffer {
    std::array<Slot<T>, N>   slots;
    std::atomic<uint64_t>    head;   // producer cursor (MPSC: CAS)
    uint64_t                 tail;   // consumer cursor (single consumer, relaxed)
    OverflowPolicy           policy; // DROP_OLDEST | DROP_NEWEST | BLOCK
};
```

This is a Dmitry Vyukov-style MPSC queue. Producers do a single CAS on `head`; the consumer reads from `tail` without any synchronization with other consumers (there is exactly one consumer — the dispatch thread).

Default capacities (configurable at `Core` construction):
- LogBuffer: 65536 slots
- MetricBuffer: 16384 slots
- ProgressBuffer: 4096 slots

---

## Dispatch Thread

The dispatch thread wakes on a condition variable or spins (configurable) when the ring buffers are non-empty. It drains all three buffers in priority order (Progress → Metric → Log, to keep the dashboard responsive) and routes records to sinks.

### Predicate evaluation

Each sink holds a compiled predicate. The dispatch thread evaluates the predicate against the record's tag set. Predicate evaluation is a tree walk over a small in-memory structure — no string parsing, no regex, no allocation.

Predicate node types:
- `TagEquals(key, value)` — O(N) scan of tag set, N ≤ 16
- `LevelAtLeast(min_level)` — one integer comparison
- `And(left, right)` — short-circuit
- `Or(left, right)` — short-circuit
- `Not(inner)`
- `Always` / `Never` — constants

Predicates are immutable after registration. Changing a sink's predicate requires deregistering and re-registering the sink, which is an infrequent operation.

---

## Sink Architecture

```cpp
class Sink {
public:
    // Called by dispatch thread. Implementations must be non-blocking.
    virtual void on_log(const LogRecord&)          {}
    virtual void on_metric(const MetricRecord&)    {}
    virtual void on_progress(const ProgressRecord&){}
    virtual void flush() = 0;
    virtual ~Sink() = default;
};
```

Sinks are registered with the `Core` along with a predicate. The `Core` owns them via `unique_ptr`. Sinks are called sequentially on the dispatch thread — they must not block. Long-running I/O (file writes, network sends) must be done on an internal sink-owned thread, with the `on_*` methods simply enqueuing into a sink-local queue.

### Built-in sinks

**TerminalSink**
- ANSI color output configurable per level (fg/bg color, bold, underline)
- Optional dashboard region rendered above the log scroll region via FTXUI
- Dashboard shows: one row per active ProgressRecord, sparkline per MetricRecord (configurable which metrics appear)
- All rendering happens on the dispatch thread; FTXUI's event loop is bypassed

**FileSink**
- Writes to a file or rotating file set
- Formatter configurable: plain text, structured key=value, or JSON
- Internal write thread with a small queue; `on_log` enqueues only

**JsonSink**
- Emits newline-delimited JSON (one object per record)
- All three record types serialized to a unified envelope with a `type` field
- Suitable for piping to `jq`, forwarding to Loki, or loading into pandas

**NullSink**
- Discards all records. Useful for benchmarking the broker overhead in isolation.

---

## Producer Macros

Macros are the primary producer API. They capture source location at the call site and expand to nothing below the compile-time level threshold.

```
LOG_TRACE(fmt, ...)
LOG_DEBUG(fmt, ...)
LOG_INFO(fmt, ...)
LOG_WARN(fmt, ...)
LOG_ERROR(fmt, ...)
LOG_FATAL(fmt, ...)   // also calls std::terminate after dispatch
```

Each macro returns a `RecordBuilder` that supports fluent `.tag(k, v)` chaining before the record is committed to the ring buffer. The builder is a stack-local object; committing moves the record into the ring without copying.

```cpp
// Expands to approximately:
#define LOG_INFO(fmt, ...)                                      \
  LUMEN_IF_ENABLED(INFO,                                        \
    ::lumen::detail::make_builder(                              \
        LogLevel::INFO,                                         \
        std::source_location::current(),                        \
        ::lumen::detail::format(fmt, ##__VA_ARGS__)             \
    )                                                           \
  )
```

`LUMEN_IF_ENABLED(LEVEL, expr)` expands to `expr` when `LEVEL >= LUMEN_MIN_LEVEL`, or to `((void)0)` otherwise. The compiler eliminates the dead branch entirely.

### Scope helpers

```
LUMEN_SCOPE(key, value, ...)      — push tags for this lexical scope
LUMEN_LOOP(label, var, total)     — progress bar + scope with iteration index
LUMEN_FRAME_SCOPE(frame_var)      — scope carrying frame_number and delta_time
LUMEN_MEMBER_SCOPE                — activate this instance's tags for this method
```

---

## Core Singleton

`lumen::Core` is the central object. It owns the ring buffers, the dispatch thread, and the registered sinks. It is a Meyer's singleton accessed via `lumen::core()`. It may also be constructed explicitly for embedded use cases (multiple independent cores in one process, e.g., one per plugin).

```cpp
auto& core = lumen::core();
core.set_process_tag("job_id", slurm_job_id());
core.set_process_tag("host",   hostname());
core.add_sink(std::make_unique<TerminalSink>(), level_at_least(INFO));
core.add_sink(std::make_unique<FileSink>("run.log"), always());
```

---

## Thread Safety Model

| Operation | Thread | Synchronization |
|---|---|---|
| Emit record (producer) | Any | CAS on ring buffer head |
| Tag merge | Producer thread | Thread-local reads only |
| Scope push/pop | Producer thread | Thread-local only |
| Drain buffer (consumer) | Dispatch thread | Single consumer, relaxed reads |
| Predicate evaluation | Dispatch thread | Read-only after registration |
| Add/remove sink | Any (infrequent) | Mutex on sink list, write lock |
| Flush | Any | Sink-specific |

The producer path touches no shared mutable state except the ring buffer head CAS. This makes it suitable for use inside tight loops including game frame update and HEP event processing.

---

## Memory Model

No heap allocation on the emit path for the common case:
- Tag values that are string literals: zero copy, string\_view into rodata
- Formatted message strings: written into a thread-local arena (default 4 KB per thread)
- Tag values from variables: formatted into the same arena
- The arena is reset after the dispatch thread finishes reading the record

Arena overflow (record + tags exceeding 4 KB) falls back to a `std::string` allocation. This is treated as an exceptional case and logs a warning to stderr.

---

## CMake Feature Flags

| Flag | Default | Effect |
|---|---|---|
| `LUMEN_MIN_LEVEL` | `DEBUG` | Elides records below this level at compile time |
| `LUMEN_ENABLE_DASHBOARD` | `ON` | Includes FTXUI dependency; enables dashboard rendering |
| `LUMEN_ENABLE_LATEX` | `OFF` | Includes MicroTeX; enables LaTeX math rendering in sinks |
| `LUMEN_ENABLE_PYTHON` | `OFF` | Builds pybind11 bindings |
| `LUMEN_ENABLE_REFLECTION` | `AUTO` | Enables C++26 class-name harvesting when compiler supports it |
| `LUMEN_ARENA_SIZE_KB` | `4` | Per-thread arena size in KB |
| `LUMEN_LOG_CAPACITY` | `65536` | LogBuffer slot count (must be power of two) |
| `LUMEN_METRIC_CAPACITY` | `16384` | MetricBuffer slot count |
| `LUMEN_PROGRESS_CAPACITY` | `4096` | ProgressBuffer slot count |

---

## Bridge Architecture

A bridge is a header that specializes the `lumen::bridge::Hook<T>` template for a known type. When the user includes the bridge header, the specialization is instantiated. The specialization wraps the type's key methods (via inheritance, template specialization, or platform-specific interception) to emit records.

Bridges depend only on the producer-facing part of Lumen (the macros and `logger.metric()`). They have no dependency on sink internals. This means a bridge can be included in a translation unit that knows nothing about how records are eventually consumed.
