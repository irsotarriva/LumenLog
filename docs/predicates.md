# Predicates

Predicates control which records each sink receives. They are compiled to small decision trees at construction time (in C++ or from a text query) and evaluated on the dispatch thread — no string parsing, no regex, no allocation per record.

## Primitives

```cpp
using namespace lumen;

always()                            // Accept all records
never()                             // Accept no records
level_at_least(LogLevel::WARN)      // LogRecords with severity >= WARN
level_equals(LogLevel::ERROR)       // LogRecords with severity exactly ERROR
tag_equals("subsystem", "calo")     // Records where tag subsystem=calo is present
tag_exists("event_id")              // Records where tag key "event_id" is present (any value, even empty)
level_compare(CompareOp::LT, WARN)  // Any comparison on the level: EQ NE LT LE GT GE
```

## Numeric tag comparisons

```cpp
tag_less("altitude", 70000)          // altitude <  70000
tag_less_equal("altitude", 70000)    // altitude <= 70000
tag_greater("dv", 0.5)               // dv > 0.5
tag_greater_equal("stage", 2)        // stage >= 2
tag_compare("vessel_id", CompareOp::EQ, 42)   // numeric equality: matches "42" and "42.0"
```

The tag's value is parsed as a number (`42`, `-3.5`, `1e-06`, `inf`). If the tag is missing, or its value is not entirely a number (`"42abc"`, `""`), every comparison is **false**, including `NE`.

Numeric tags round-trip exactly: `.tag("altitude", 69999.97)` stores `"69999.97"`, and integers of any width are stored in full, so a threshold compares against the value you logged.

## Runtime queries

`parse_predicate` builds the same predicates from text, so a filter can come from a config file, a CLI flag or a console and be changed without recompiling:

```cpp
auto parsed = lumen::parse_predicate("level >= WARN && vessel_id == 42 && !exists(suppress)");
if (!parsed) {
    // parsed.error().position: byte offset of the problem
    // parsed.error().message:  e.g. "expected a level (TRACE, DEBUG, INFO, WARN, ERROR, FATAL)"
    // parsed.error().code:     LumenError::invalid_predicate
    return;
}
core.add_sink(std::make_unique<FileSink>(cfg), std::move(*parsed));
```

It returns `std::expected<Predicate, PredicateParseError>` and never throws.

| Syntax | Meaning |
|---|---|
| `level >= WARN` | level comparison (`== != < <= > >=`); levels are `TRACE`…`FATAL` (any case) or `0`–`5` |
| `altitude < 70000` | numeric comparison (as `tag_compare`) |
| `vessel_id == 42` | numeric equality — a number on the right always compares numerically |
| `phase == ascent`, `phase == "ascent"` | text equality; `!=` also allowed; `< > <= >=` need a number |
| `exists(suppress)` | tag present (any value) |
| `&&` / `and`, `\|\|` / `or`, `!` / `not`, `( )` | boolean structure; `&&` binds tighter than `\|\|` |
| `true`, `false` | constants |
| `"level" == 3`, `exists("and")` | quote a key that collides with a keyword |

Keys are identifiers (letters, digits, `_`, `.`), e.g. `vessel.id`. Strings take `"…"` or `'…'` with `\` escapes.

Any comparison against a missing tag is false. To let records without the tag through, say so: `!exists(phase) || phase != orbit`.

Queries nested more than 64 levels deep are rejected, so a query typed into a console cannot exhaust the stack.

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
