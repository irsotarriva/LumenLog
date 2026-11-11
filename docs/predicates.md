# Predicates

A `lumen::Predicate` decides which records a sink receives. It is given to
`Core::add_sink` and fixed for as long as the sink is registered. The
reference is `lumen/predicate.hpp`.

## Building predicates in C++

<!-- check: prelude=common wrap=body -->
```cpp
using lumen::LogLevel;

const lumen::Predicate warnings = lumen::level_at_least(LogLevel::Warn);
const lumen::Predicate calorimeter = lumen::tag_equals("detector", "calorimeter");
const lumen::Predicate hot = lumen::tag_greater("temperature", 300.0);
const lumen::Predicate quiet = lumen::tag_exists("suppress");

const lumen::Predicate route = (warnings && calorimeter) || (hot && !quiet);

auto sink = lumen::TerminalSink::create({});
if (sink) {
    if (const auto id = lumen::core().add_sink(std::move(*sink), route); !id) {
        LUMEN_LOG_ERROR("sink not registered: {}", id.error().message());
    }
}
```

| Function | True for a record that ... |
|---|---|
| `always()`, `never()` | every record; no record |
| `level_at_least(l)` | is a log record with level ≥ `l` |
| `level_equals(l)` | is a log record with level `l` |
| `level_compare(op, l)` | is a log record with `level op l`; `op` is a `CompareOp`: `Equal`, `NotEqual`, `Less`, `LessEqual`, `Greater`, `GreaterEqual` |
| `tag_equals(k, v)` | has tag `k` with exactly the text `v` (byte for byte) |
| `tag_not_equals(k, v)` | has tag `k` with a text other than `v` |
| `tag_exists(k)` | has tag `k`, whatever its value (an empty value counts) |
| `tag_compare(k, op, x)` | has tag `k` whose value is a number `n` with `n op x` |
| `tag_less`, `tag_less_equal`, `tag_greater`, `tag_greater_equal` | `tag_compare` with that operator |

`a && b`, `a || b` and `!a` combine predicates. A default-constructed
`Predicate` matches every record, and `add_sink` uses `always()` when no
predicate is given.

## Parsing predicates from text

`lumen::parse_predicate` builds the same predicates from a string, so that a
filter can come from a configuration file or a command-line flag. It returns
`std::expected<Predicate, PredicateParseError>`; the error holds the code
`LumenError::InvalidPredicate`, the byte offset of the problem and a message.

<!-- check: prelude=common wrap=body -->
```cpp
const auto parsed = lumen::parse_predicate("level >= WARN && run == 42 && !exists(suppress)");
if (!parsed) {
    std::cerr << "invalid filter at offset " << parsed.error().position << ": "
              << parsed.error().message << '\n';
    return;
}
const lumen::Predicate filter = *parsed;
```

The grammar, as stated in `lumen/predicate.hpp` (whitespace is free; keywords
are case-insensitive):

<!-- check: lines-in=include/lumen/predicate.hpp -->
```text
query      := or
or         := and { ("||" | "or") and }
and        := unary { ("&&" | "and") unary }
unary      := ("!" | "not") unary | primary
primary    := "(" query ")" | "true" | "false"
            | "exists" "(" key ")"
            | "level" op LEVEL
            | key op value
op         := "==" | "!=" | "<" | "<=" | ">" | ">="
key        := identifier | quoted string      (quote a key named e.g. "level")
identifier := ASCII letter or '_', then ASCII letters, digits, '_' or '.'
value      := number | quoted string | identifier
LEVEL      := TRACE | DEBUG | INFO | WARN | ERROR | FATAL | 0..5
```

- Strings take single or double quotes; a backslash makes the next character
  literal. Character classes are ASCII and do not depend on the C locale.
- A number on the right compares numerically, as `tag_compare`: `id == 42`
  matches the tag values `42` and `42.0`. A string or a bare word compares
  text, as `tag_equals` and `tag_not_equals`, and only `==` and `!=` are
  allowed with it.
- `&&` binds tighter than `||`.
- Parentheses and `!` together may nest at most 63 deep; a deeper query is a
  parse error.

## The rules

These hold for every predicate, built or parsed:

1. **A comparison against a missing tag is false**, whatever the operator
   (`!=` included) and whatever the value (`""` included). Write
   `!tag_exists(k) || ...` (or `!exists(k) || ...`) when a record without
   the tag should pass.
2. **`!` is plain negation.** `!tag_equals("k", "v")` is true for a record
   that has no `k`. `tag_not_equals("k", "v")` is false for it (rule 1).
3. **Numeric comparisons** parse the tag's value as a number: `70000`,
   `-3.5` and `1e-6` are numbers; `+1`, ` 1` and `42abc` are not. A value
   that is not entirely a number makes every numeric comparison false,
   `NotEqual` included.
4. **Text comparisons** compare bytes. `tag_equals("n", "42")` does not match
   the value `42.0`.
5. **Duplicate keys.** If a record holds a key twice, the first entry is the
   tag's value. Records that went through a Core never hold a key twice; the
   rule matters only for a `TagSet` you fill yourself.
6. **Metrics and progress records have no level.** For them every level
   condition is ignored, as if it were absent, and the tag conditions decide.
   `level >= WARN` passes every metric; `level >= WARN && run == 42` and
   `level >= WARN || run == 42` both pass exactly the metrics with `run == 42`;
   a predicate made only of level conditions passes every metric and progress
   record. A sink cannot select record types with its predicate: a sink that
   wants only log records overrides only `on_log`.

This program checks each rule; ctest runs it.

<!-- check: prelude=common -->
```cpp
int main() {
    using lumen::LogLevel;
    lumen::RecordTags tags;
    if (!tags.add("run", "42.0") || !tags.add("name", "") || !tags.add("count", "42abc")) {
        return 1;
    }
    const auto passes = [&](const lumen::Predicate& p) { return p.evaluate(tags, LogLevel::Info); };

    const bool rule1 = !passes(lumen::tag_not_equals("missing", "x")) &&
                       !passes(lumen::tag_equals("missing", "")) &&
                       !passes(lumen::tag_compare("missing", lumen::CompareOp::NotEqual, 1.0));
    const bool rule2 = passes(!lumen::tag_equals("missing", "x"));
    const bool rule3 = passes(lumen::tag_compare("run", lumen::CompareOp::Equal, 42.0)) &&
                       !passes(lumen::tag_compare("count", lumen::CompareOp::NotEqual, 0.0));
    const bool rule4 = !passes(lumen::tag_equals("run", "42"));
    const lumen::Predicate either = lumen::level_at_least(LogLevel::Warn) ||
                                    lumen::tag_greater("run", 100.0);
    const bool rule6 = lumen::level_at_least(LogLevel::Warn).evaluate(tags) &&
                       !either.evaluate(tags) && !passes(either);

    const auto parsed = lumen::parse_predicate("run == 42 && exists(name)");
    const bool parsing = parsed && passes(*parsed) && !lumen::parse_predicate("run < high");
    return rule1 && rule2 && rule3 && rule4 && rule6 && parsing ? 0 : 1;
}
```

## How a predicate is evaluated

A `Predicate` is an immutable expression tree. It is not compiled to another
form: each evaluation walks the tree, on the thread that delivers records,
once per record and sink. `&&` and `||` short-circuit: in `a && b`, `b` is
not evaluated for a record that `a` rejects. (Building `a && b` in C++
evaluates both operand expressions, as for any overloaded operator.) The walk
does not recurse, so a tree of any depth is safe, and it allocates nothing
unless the tree is more than 128 levels deep. A tag lookup is a linear scan of
the record's at most 16 tags; a numeric condition parses the tag's text on
every evaluation.

Nodes are shared, never copied: copying a predicate or combining two costs at
most one small allocation. A `Predicate` may be evaluated by several threads
at once.

To change the filter of a registered sink, remove the sink and add it again:

<!-- check: prelude=common wrap=body -->
```cpp
const auto id = lumen::core().add_sink(std::make_unique<lumen::NullSink>());
if (!id) {
    return;
}
auto sink = lumen::core().remove_sink(*id);
if (sink) {
    const auto again = lumen::core().add_sink(std::move(*sink), lumen::tag_exists("run"));
    if (!again) {
        LUMEN_LOG_ERROR("sink not registered again: {}", again.error().message());
    }
}
```

Records dispatched between the two calls do not reach the sink.
