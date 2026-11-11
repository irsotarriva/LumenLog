#ifndef LUMEN_PREDICATE_HPP
#define LUMEN_PREDICATE_HPP

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

#include "lumen/record.hpp"

namespace lumen {

enum class CompareOp : uint8_t {
    Equal,         // ==
    NotEqual,      // !=
    Less,          // <
    LessEqual,     // <=
    Greater,       // >
    GreaterEqual,  // >=
};

namespace detail {
struct PredicateAccess;
}  // namespace detail

// A Predicate decides which records a sink receives. It is an immutable
// expression tree of conditions on a record's level and tags, joined by
// `&&`, `||` and `!`:
//
//   core.add_sink(std::move(sink),
//                 level_at_least(LogLevel::Warn) && tag_equals("subsystem", "cooling"));
//
// The tree is not compiled to another form: evaluation walks it. Nodes are
// shared, never copied, so copying a Predicate or combining two costs one
// small allocation at most, whatever their size. Evaluation does not recurse,
// so a tree of any depth is safe, and it allocates nothing unless the tree is
// more than 128 levels deep. A Predicate may be evaluated from several
// threads at once.
//
// Rules shared by every condition:
//   - A record's tags may hold the same key more than once; the first entry
//     with that key is the tag's value, and later ones are never looked at.
//   - Any comparison against a missing tag is false, whatever the operator
//     (`!=` included) and whatever the value (`""` included). Use
//     `!tag_exists(k) || ...` when a missing tag should pass.
//   - `!p` is plain logical negation, so `!tag_equals("k", "v")` is true for a
//     record without `k`.
//
// A default-constructed Predicate matches every record.
class Predicate {
public:
    Predicate() = default;

    // For log records.
    [[nodiscard]] bool evaluate(const TagSet<16>& tags, LogLevel level) const;

    // For records without a level (metrics, progress): level conditions are
    // ignored, as if absent from the predicate, and the tag conditions decide.
    // So `level >= Warn` passes every metric, `level >= Warn && run_id == 42`
    // passes metrics with run_id 42, and `level >= Warn || run_id == 42`
    // passes only those too. A predicate made only of level conditions passes
    // every such record.
    [[nodiscard]] bool evaluate(const TagSet<16>& tags) const;

    // Defined in predicate.cpp; users never see a complete Node.
    struct Node;

private:
    friend struct detail::PredicateAccess;
    explicit Predicate(std::shared_ptr<const Node> node);

    std::shared_ptr<const Node> node_;  // null: matches every record
};

// ── Building predicates ──────────────────────────────────────────────────────

[[nodiscard]] Predicate always();
[[nodiscard]] Predicate never();

// `record level <op> level`, e.g. level_compare(CompareOp::Less, LogLevel::Warn)
// keeps Trace, Debug and Info.
[[nodiscard]] Predicate level_compare(CompareOp op, LogLevel level);
// level_compare(CompareOp::GreaterEqual, min_level).
[[nodiscard]] Predicate level_at_least(LogLevel min_level);
// level_compare(CompareOp::Equal, level).
[[nodiscard]] Predicate level_equals(LogLevel level);

// Text comparison of the tag's value, byte for byte. False if the tag is missing.
[[nodiscard]] Predicate tag_equals(std::string_view key, std::string_view value);
[[nodiscard]] Predicate tag_not_equals(std::string_view key, std::string_view value);

// True if the record has the tag, whatever its value (an empty value counts).
[[nodiscard]] Predicate tag_exists(std::string_view key);

// Numeric comparison: the tag's value is parsed as a number (e.g. "70000",
// "-3.5", "1e-6"; not "+1", " 1" or "42abc") and compared with `value`. A
// missing tag or a value that is not entirely a number makes every comparison
// false, NotEqual included.
[[nodiscard]] Predicate tag_compare(std::string_view key, CompareOp op, double value);
[[nodiscard]] Predicate tag_less(std::string_view key, double value);
[[nodiscard]] Predicate tag_less_equal(std::string_view key, double value);
[[nodiscard]] Predicate tag_greater(std::string_view key, double value);
[[nodiscard]] Predicate tag_greater_equal(std::string_view key, double value);

// ── Combining predicates ─────────────────────────────────────────────────────
//
// The resulting predicate short-circuits when it is evaluated: in `a && b`,
// `b` is not looked at for a record that `a` rejects. Building it does not:
// these are overloaded operators, so both operand expressions are always
// evaluated in C++, like the arguments of any function. A default-constructed
// operand acts as always().

[[nodiscard]] Predicate operator&&(const Predicate& lhs, const Predicate& rhs);
[[nodiscard]] Predicate operator||(const Predicate& lhs, const Predicate& rhs);
[[nodiscard]] Predicate operator!(const Predicate& operand);

// ── Runtime queries ──────────────────────────────────────────────────────────
//
// parse_predicate() builds a Predicate from text, so filters can come from a
// config file, a CLI flag or a console without recompiling:
//
//   level >= WARN && run_id == 42 && !exists(suppress)
//   (stage == "warmup" || stage == steady) && temperature < 350
//
// Grammar (whitespace is free; keywords are case-insensitive):
//   query      := or
//   or         := and { ("||" | "or") and }
//   and        := unary { ("&&" | "and") unary }
//   unary      := ("!" | "not") unary | primary
//   primary    := "(" query ")" | "true" | "false"
//               | "exists" "(" key ")"
//               | "level" op LEVEL
//               | key op value
//   op         := "==" | "!=" | "<" | "<=" | ">" | ">="
//   key        := identifier | quoted string      (quote a key named e.g. "level")
//   identifier := ASCII letter or '_', then ASCII letters, digits, '_' or '.'
//   value      := number | quoted string | identifier
//   LEVEL      := TRACE | DEBUG | INFO | WARN | ERROR | FATAL | 0..5
//
// Strings take single or double quotes; a backslash makes the next character
// literal. Character classes are ASCII and do not depend on the C locale.
//
// A number on the right compares numerically, as tag_compare (so `id == 42`
// matches "42" and "42.0"); a string or bare word compares text, as
// tag_equals / tag_not_equals, and only == and != are allowed with it. The
// rules above apply: a comparison against a missing tag is false, and the
// first of duplicate keys decides. Parentheses and `!` together may nest at
// most 63 deep; deeper queries are a parse error.

struct PredicateParseError {
    std::error_code code;  // LumenError::InvalidPredicate
    size_t position = 0;   // byte offset into the query where the error was found
    std::string message;   // human-readable, e.g. "expected ')'"
};

[[nodiscard]] std::expected<Predicate, PredicateParseError> parse_predicate(std::string_view query);

}  // namespace lumen

#endif
