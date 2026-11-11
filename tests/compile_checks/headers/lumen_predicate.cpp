// lumen/predicate.hpp alone declares what it documents, including the error
// code a parse error carries.
#include "lumen/predicate.hpp"

#include <type_traits>

namespace lumen {

static_assert(std::is_same_v<decltype(always()), Predicate>);
static_assert(std::is_same_v<decltype(never()), Predicate>);
static_assert(std::is_same_v<decltype(level_compare(CompareOp::Less, LogLevel::Warn)), Predicate>);
static_assert(std::is_same_v<decltype(level_at_least(LogLevel::Warn)), Predicate>);
static_assert(std::is_same_v<decltype(level_equals(LogLevel::Warn)), Predicate>);
static_assert(std::is_same_v<decltype(tag_equals("k", "v")), Predicate>);
static_assert(std::is_same_v<decltype(tag_not_equals("k", "v")), Predicate>);
static_assert(std::is_same_v<decltype(tag_exists("k")), Predicate>);
static_assert(std::is_same_v<decltype(tag_compare("k", CompareOp::GreaterEqual, 1.0)), Predicate>);
static_assert(std::is_same_v<decltype(tag_less("k", 1.0)), Predicate>);
static_assert(std::is_same_v<decltype(tag_less_equal("k", 1.0)), Predicate>);
static_assert(std::is_same_v<decltype(tag_greater("k", 1.0)), Predicate>);
static_assert(std::is_same_v<decltype(tag_greater_equal("k", 1.0)), Predicate>);
static_assert(std::is_same_v<decltype(always() && never()), Predicate>);
static_assert(std::is_same_v<decltype(always() || never()), Predicate>);
static_assert(std::is_same_v<decltype(!always()), Predicate>);
static_assert(std::is_same_v<decltype(Predicate{}.evaluate(RecordTags{}, LogLevel::Info)), bool>);
static_assert(std::is_same_v<decltype(Predicate{}.evaluate(RecordTags{})), bool>);
static_assert(std::is_same_v<decltype(parse_predicate("level >= WARN")),
                             std::expected<Predicate, PredicateParseError>>);
static_assert(
    std::is_same_v<decltype(PredicateParseError{}.code == LumenError::InvalidPredicate), bool>);
static_assert(std::is_same_v<decltype(PredicateParseError{}.position), std::size_t>);
static_assert(std::is_same_v<decltype(PredicateParseError{}.message), std::string>);

}  // namespace lumen
