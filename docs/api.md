# API Reference

## Headers

| Header | Contains |
|---|---|
| `lumen/lumen.h` | Umbrella header — everything you typically need |
| `lumen/core.h` | `Core` singleton, sink management, process/thread tags |
| `lumen/record.h` | Record types, builders, macros, scope helpers |
| `lumen/sink.h` | `Sink` base class, `TerminalSink`, `FileSink`, `JsonSink`, `NullSink` |
| `lumen/predicate.h` | Predicate DSL |
| `lumen/tagged.h` | `Tagged` mixin for instance-level tags |

## Core

### `lumen::core()`

Returns the global `Core` singleton (Meyer's singleton).

```cpp
auto& core = lumen::core();
```

### `Core::add_sink`

```cpp
SinkId add_sink(std::unique_ptr<Sink> sink, Predicate predicate);
```

Register a sink with a filter predicate. Returns an opaque `SinkId` for later removal.

### `Core::remove_sink`

```cpp
std::expected<std::unique_ptr<Sink>, std::error_code> remove_sink(SinkId id);
```

Remove a previously registered sink. Returns the sink's `unique_ptr` on success.

### `Core::set_process_tag`

```cpp
void set_process_tag(std::string_view key, std::string_view value);
```

Set a tag attached to every record from this process. Call at startup.

### `Core::set_thread_tag`

```cpp
void set_thread_tag(std::string_view key, std::string_view value);
```

Set a tag attached to every record from the calling thread. Call at thread creation.

### `Core::flush`

```cpp
void flush();
```

Block until every record emitted before the call has been delivered to the sinks and each sink's `flush()` has returned (for `FileSink`/`JsonSink`: the data has been written to the file). Pending records are drained on the calling thread, serialised with the dispatch thread, so sinks are never called concurrently. Calling `flush()` from inside a sink callback is a no-op.

## Record types

### `LogRecord`

```cpp
struct LogRecord {
    LogLevel         level;          // TRACE=0, DEBUG=1, INFO=2, WARN=3, ERROR=4, FATAL=5
    std::string_view message;
    TagSet<16>       tags;
    uint64_t         timestamp_ns;
    uint32_t         thread_id;
    SourceLocation   source;         // { file, function, class_name, line }
    std::shared_ptr<const char[]> storage;  // owns message and tag strings
};
```

Every record owns its strings: when a record is emitted, the message (or metric name, or progress label) and all tag keys and values are copied into `storage`, a NUL-terminated block shared by the record's copies. It is therefore safe to log temporaries (`LOG_INFO(std::format(...))`, `.tag("k", some_string)`), and a sink may keep a copy of a record for as long as it likes. `SourceLocation` strings are static and are not copied. The same applies to `MetricRecord` and `ProgressRecord`.

### `MetricRecord`

```cpp
struct MetricRecord {
    std::string_view name;
    double           value;
    TagSet<16>       tags;
    uint64_t         timestamp_ns;
    std::shared_ptr<const char[]> storage;
};
```

### `ProgressRecord`

```cpp
struct ProgressRecord {
    uint64_t         id;
    std::string_view label;
    uint64_t         current;
    uint64_t         total;
    TagSet<16>       tags;
    uint64_t         timestamp_ns;
    std::shared_ptr<const char[]> storage;
};
```

When `current == total`, the record signals completion.

### `TagSet<N>`

Fixed-capacity key-value container. Thread-safe for reads, not for concurrent writes.

```cpp
template<size_t N>
struct TagSet {
    bool        add(std::string_view key, std::string_view value);
    const char* find(std::string_view key) const;
    bool        has(std::string_view key) const;
    size_t      count() const;
    // iterable via begin() / end()
};
```

## Log macros

```cpp
LOG_TRACE(fmt, ...)    // Severity 0
LOG_DEBUG(fmt, ...)    // Severity 1
LOG_INFO(fmt, ...)     // Severity 2
LOG_WARN(fmt, ...)     // Severity 3
LOG_ERROR(fmt, ...)    // Severity 4
LOG_FATAL(fmt, ...)    // Severity 5 — calls std::terminate after dispatch
```

With a single argument the message is logged verbatim (no format parsing, so runtime strings and braces are fine). With more arguments, `fmt` is a `std::format` string checked at compile time. Each macro returns `RecordBuilder&` for `.tag()` chaining. `LOG_FATAL` flushes all sinks before terminating.

Below `LUMEN_MIN_LEVEL`, macros expand to `((void)0)`.

## Record builder

```cpp
struct RecordBuilder {
    RecordBuilder& tag(std::string_view key, std::string_view value);
    RecordBuilder& tag(std::string_view key, int64_t value);
    RecordBuilder& tag(std::string_view key, double value);
    // Destructor commits the record to the ring buffer
};
```

## Metric emission

```cpp
MetricBuilder& metric(std::string_view name, double value);
```

Returns `MetricBuilder&` with the same `.tag()` interface. Never elided by `LUMEN_MIN_LEVEL`.

## Progress emission

```cpp
ProgressHandle progress(std::string_view label, uint64_t total);
```

### `ProgressHandle`

```cpp
struct ProgressHandle {
    void update(uint64_t position);  // Set absolute position
    void tick();                     // Increment by 1
    void finish();                   // Mark complete
    // Destructor calls finish() if not already finished
};
```

## Scope macros

```cpp
LUMEN_SCOPE(key, value, ...)           // Push tags for current scope (RAII)
LUMEN_LOOP(label, var, total)          // Progress bar + scope tags for loop
LUMEN_FRAME_SCOPE(frame, delta_ms)     // Frame-aware scope
LUMEN_MEMBER_SCOPE                     // Activate instance tags for method
```

All scope macros are RAII — safe under exceptions and early returns.

## Tagged mixin

```cpp
class Tagged {
protected:
    void lumen_tag(std::string_view key, std::string_view value);
    void lumen_tag(std::string_view key, int64_t value);
    void lumen_tag(std::string_view key, double value);
    [[nodiscard]] Scope lumen_scope();
    const TagSet<8>& tags() const;
};
```

Store up to 8 instance-level tags. Use `LUMEN_MEMBER_SCOPE` to activate them for a method call.

## Sink base class

```cpp
class Sink {
public:
    virtual void on_log(const LogRecord&)     {}
    virtual void on_metric(const MetricRecord&) {}
    virtual void on_progress(const ProgressRecord&) {}
    virtual void flush() = 0;
    virtual ~Sink() = default;
};
```

`on_*` methods are called from the dispatch thread and must be non-blocking.

## Predicate DSL

```cpp
Predicate always();
Predicate never();
Predicate level_at_least(LogLevel min_level);
Predicate level_equals(LogLevel level);
Predicate tag_equals(std::string_view key, std::string_view value);
Predicate tag_exists(std::string_view key);

Predicate operator&&(Predicate a, Predicate b);
Predicate operator||(Predicate a, Predicate b);
Predicate operator!(Predicate a);
```

## Built-in sinks

### TerminalSink

```cpp
struct TerminalSink::Config {
    AnsiColor colors[6];              // Per-level colors
    std::string time_format;          // strftime format
    std::vector<std::string_view> inline_tags;
    std::vector<std::string_view> dashboard_metrics;
    int dashboard_height;
};

static Config default_config();
```

### FileSink

```cpp
struct FileSink::Config {
    std::string path;                 // Supports {date} substitution
    size_t rotate_mb;                 // 0 = no rotation
    int max_files;                    // Keep last N rotated files
    FileFormat format;                // PlainText, KeyValue, Json
};
```

### JsonSink

```cpp
explicit JsonSink(std::string path);
```

### NullSink

```cpp
NullSink();  // Discards all records
```
