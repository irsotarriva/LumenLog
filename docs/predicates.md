# Predicates

Predicates control which records each sink receives. They are compiled to small decision trees at construction time and evaluated on the dispatch thread — no string parsing, no regex, no allocation.

## Primitives

```cpp
using namespace lumen;

always()                            // Accept all records
never()                             // Accept no records
level_at_least(LogLevel::WARN)      // LogRecords with severity >= WARN
level_equals(LogLevel::ERROR)       // LogRecords with severity exactly ERROR
tag_equals("subsystem", "calo")     // Records where tag subsystem=calo is present
tag_exists("event_id")              // Records where tag key "event_id" is present (any value)
```

`level_at_least` and `level_equals` match only `LogRecord` types. They are inert for `MetricRecord` and `ProgressRecord` — those record types always pass level checks.

## Composition

Combine primitives with operators:

```cpp
// AND — both conditions must match
level_at_least(WARN) && tag_equals("subsystem", "calorimeter")

// OR — either condition matches
tag_equals("run", "42") || tag_equals("run", "43")

// NOT — invert a predicate
!tag_exists("suppress")

// Compound
level_at_least(INFO) && tag_equals("subsystem", "tracking") && !tag_exists("suppress")
```

## Evaluation

Predicate evaluation is a tree walk. Nodes short-circuit:
- `AND` skips right child if left is false
- `OR` skips right child if left is true
- `NOT` inverts its child's result

Tag lookups are linear scans over a fixed-capacity array (max 16 tags) — faster than a hash map at this size due to cache locality.

## Immutability

Predicates are value types. They are copied into the sink at registration time and cannot be changed afterward. To change a sink's filter, remove the sink and re-register with a new predicate:

```cpp
auto old = core.remove_sink(id);       // returns the sink unique_ptr
core.add_sink(std::move(*old), new_predicate);
```

## Examples

### Route by severity

```cpp
// Errors go to stderr
core.add_sink(std::make_unique<TerminalSink>(), level_at_least(ERROR));

// Everything goes to file
core.add_sink(std::make_unique<FileSink>("debug.log"), always());
```

### Route by subsystem

```cpp
core.add_sink(std::make_unique<FileSink>("calo.log"),
    tag_equals("subsystem", "calorimeter"));

core.add_sink(std::make_unique<FileSink>("tracking.log"),
    tag_equals("subsystem", "tracking"));
```

### Combined

```cpp
// Only calorimeter warnings and above, unless suppressed
core.add_sink(std::make_unique<TerminalSink>(),
    level_at_least(WARN) && tag_equals("subsystem", "calorimeter") && !tag_exists("suppress"));

// All metrics regardless of source
core.add_sink(std::make_unique<JsonSink>("metrics.jsonl"), always());
```

### Record type filtering

Sinks filter by which `on_*` methods they implement, not the predicate. If a sink only overrides `on_log`, metric and progress records are never delivered to it even if the predicate matches. If a sink implements all three, the same predicate is evaluated for all record types.
