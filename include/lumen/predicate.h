#ifndef LUMEN_PREDICATE_H
#define LUMEN_PREDICATE_H

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

#include "lumen/record.h"

namespace lumen {

enum class CompareOp : uint8_t {
    EQ,  // ==
    NE,  // !=
    LT,  // <
    LE,  // <=
    GT,  // >
    GE,  // >=
};

class Predicate {
public:
    // For log records.
    [[nodiscard]] bool evaluate(const TagSet<16>& tags, LogLevel level) const;
    // For records without a level (metrics, progress): level conditions are
    // ignored, as if absent from the predicate, and the tag conditions decide.
    // So `level >= WARN` passes every metric, and
    // `level >= WARN && vessel_id == 42` passes metrics with vessel_id 42.
    [[nodiscard]] bool evaluate(const TagSet<16>& tags) const;

    Predicate();
    Predicate(const Predicate& other);
    Predicate(Predicate&&) noexcept;
    Predicate& operator=(const Predicate& other);
    Predicate& operator=(Predicate&&) noexcept;
    ~Predicate();

    friend Predicate operator&&(const Predicate& a, const Predicate& b);
    friend Predicate operator||(const Predicate& a, const Predicate& b);
    friend Predicate operator!(const Predicate& p);

    friend Predicate always();
    friend Predicate never();
    friend Predicate level_at_least(LogLevel min_level);
    friend Predicate level_equals(LogLevel level);
    friend Predicate tag_equals(std::string_view key, std::string_view value);
    friend Predicate tag_exists(std::string_view key);
    friend Predicate level_compare(CompareOp op, LogLevel level);
    friend Predicate tag_compare(std::string_view key, CompareOp op, double value);

    struct Node;

private:
    explicit Predicate(std::unique_ptr<Node> node);
    std::unique_ptr<Node> node_;
};

Predicate always();
Predicate never();
Predicate level_at_least(LogLevel min_level);
Predicate level_equals(LogLevel level);
Predicate tag_equals(std::string_view key, std::string_view value);
Predicate tag_exists(std::string_view key);

// `level <op> level`, e.g. level_compare(CompareOp::LT, WARN).
Predicate level_compare(CompareOp op, LogLevel level);

// Numeric tag comparison: the tag's value is parsed as a number (e.g. "70000",
// "-3.5", "1e-6") and compared with `value`. A missing tag or a value that is
// not entirely a number makes every comparison false, including NE.
Predicate tag_compare(std::string_view key, CompareOp op, double value);
Predicate tag_less(std::string_view key, double value);
Predicate tag_less_equal(std::string_view key, double value);
Predicate tag_greater(std::string_view key, double value);
Predicate tag_greater_equal(std::string_view key, double value);

// ── Runtime queries ──────────────────────────────────────────────────────────
//
// parse_predicate() builds a Predicate from text, so filters can come from a
// config file, a CLI flag or a console without recompiling:
//
//   level >= WARN && vessel_id == 42 && !exists(suppress)
//   (phase == "ascent" || phase == orbit) && altitude < 70000
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
//   value      := number | quoted string | identifier
//   LEVEL      := TRACE | DEBUG | INFO | WARN | ERROR | FATAL | 0..5
//
// A number on the right compares numerically (so `id == 42` matches "42" and
// "42.0"); a string or bare word compares text, and only == / != are allowed
// with it. Any comparison against a missing tag is false: write
// `!exists(k) || k != v` if a missing tag should pass.

struct PredicateParseError {
    std::error_code code;  // LumenError::invalid_predicate
    size_t position = 0;   // byte offset into the query where the error was found
    std::string message;   // human-readable, e.g. "expected ')'"
};

[[nodiscard]] std::expected<Predicate, PredicateParseError> parse_predicate(std::string_view query);

}  // namespace lumen

#endif
