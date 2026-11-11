#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "ascii.hpp"
#include "lumen/predicate.hpp"
#include "predicate_lexer.hpp"

namespace lumen {

namespace {

using detail::PredicateLexer;
using detail::Token;
using detail::TokenKind;

// Deep enough for any hand-written query; stops "((((..." from a console
// exhausting the stack.
inline constexpr uint32_t max_nesting = 64;

[[nodiscard]] constexpr bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (detail::ascii::to_lower(a[i]) != detail::ascii::to_lower(b[i])) {
            return false;
        }
    }
    return true;
}

// Indexed by the LogLevel value.
inline constexpr std::array<std::string_view, 6> level_names = {"TRACE", "DEBUG", "INFO",
                                                                "WARN",  "ERROR", "FATAL"};

[[nodiscard]] std::optional<LogLevel> parse_level(std::string_view text) {
    for (std::size_t i = 0; i < level_names.size(); ++i) {
        if (iequals(text, level_names[i])) {
            return static_cast<LogLevel>(i);
        }
    }
    if (text.size() == 1 && text[0] >= '0' && text[0] <= '5') {
        return static_cast<LogLevel>(text[0] - '0');
    }
    return std::nullopt;
}

// Recursive descent over the grammar in predicate.hpp, one function per rule.
// Each parse function starts at the current token and, on success, leaves the
// first token after its rule current. On failure it returns nullopt and
// `error_` says why.
class Parser {
public:
    explicit Parser(std::string_view query) : lexer_(query) {}

    [[nodiscard]] std::expected<Predicate, PredicateParseError> parse() {
        if (!advance_()) {
            return std::unexpected(std::move(error_));
        }
        std::optional<Predicate> result = parse_or_(0);
        if (!result) {
            return std::unexpected(std::move(error_));
        }
        if (token_.kind != TokenKind::End) {
            return std::unexpected(
                detail::make_parse_error(token_.position, "unexpected '" + token_.text + "'"));
        }
        return std::move(*result);
    }

private:
    [[nodiscard]] std::optional<Predicate> parse_or_(uint32_t depth) {
        std::optional<Predicate> lhs = parse_and_(depth);
        while (lhs && is_keyword_or_(TokenKind::Or, "or")) {
            if (!advance_()) {
                return std::nullopt;
            }
            std::optional<Predicate> rhs = parse_and_(depth);
            if (!rhs) {
                return std::nullopt;
            }
            lhs = *lhs || *rhs;
        }
        return lhs;
    }

    [[nodiscard]] std::optional<Predicate> parse_and_(uint32_t depth) {
        std::optional<Predicate> lhs = parse_unary_(depth);
        while (lhs && is_keyword_or_(TokenKind::And, "and")) {
            if (!advance_()) {
                return std::nullopt;
            }
            std::optional<Predicate> rhs = parse_unary_(depth);
            if (!rhs) {
                return std::nullopt;
            }
            lhs = *lhs && *rhs;
        }
        return lhs;
    }

    [[nodiscard]] std::optional<Predicate> parse_unary_(uint32_t depth) {
        if (depth >= max_nesting) {
            return fail_(token_.position, "query nested too deeply");
        }
        if (is_keyword_or_(TokenKind::Not, "not")) {
            if (!advance_()) {
                return std::nullopt;
            }
            std::optional<Predicate> operand = parse_unary_(depth + 1);
            if (!operand) {
                return std::nullopt;
            }
            return !*operand;
        }
        return parse_primary_(depth);
    }

    [[nodiscard]] std::optional<Predicate> parse_primary_(uint32_t depth) {
        switch (token_.kind) {
            case TokenKind::LParen:     return parse_group_(depth);
            case TokenKind::Identifier: return parse_word_condition_();
            // A quoted key, e.g. "level" == 3.
            case TokenKind::String: return parse_tag_comparison_();
            case TokenKind::End:    return fail_(token_.position, "unexpected end of query");
            default:
                return fail_(token_.position, "expected a condition, got '" + token_.text + "'");
        }
    }

    // "(" query ")"
    [[nodiscard]] std::optional<Predicate> parse_group_(uint32_t depth) {
        if (!advance_()) {
            return std::nullopt;
        }
        std::optional<Predicate> inner = parse_or_(depth + 1);
        if (!inner) {
            return std::nullopt;
        }
        if (!expect_(TokenKind::RParen, "')'")) {
            return std::nullopt;
        }
        return inner;
    }

    // A condition that starts with a bare word: a literal, a keyword, or a key.
    [[nodiscard]] std::optional<Predicate> parse_word_condition_() {
        if (iequals(token_.text, "true") || iequals(token_.text, "false")) {
            return parse_literal_();
        }
        if (iequals(token_.text, "exists")) {
            return parse_exists_();
        }
        if (iequals(token_.text, "level")) {
            return parse_level_comparison_();
        }
        return parse_tag_comparison_();
    }

    [[nodiscard]] std::optional<Predicate> parse_literal_() {
        const bool value = iequals(token_.text, "true");
        if (!advance_()) {
            return std::nullopt;
        }
        return value ? always() : never();
    }

    // "exists" "(" key ")"
    [[nodiscard]] std::optional<Predicate> parse_exists_() {
        if (!advance_()) {
            return std::nullopt;
        }
        if (!expect_(TokenKind::LParen, "'(' after exists")) {
            return std::nullopt;
        }
        if (token_.kind != TokenKind::Identifier && token_.kind != TokenKind::String) {
            return fail_(token_.position, "expected a tag name in exists(...)");
        }
        const std::string key = token_.text;
        if (!advance_()) {
            return std::nullopt;
        }
        if (!expect_(TokenKind::RParen, "')'")) {
            return std::nullopt;
        }
        return tag_exists(key);
    }

    // "level" op LEVEL
    [[nodiscard]] std::optional<Predicate> parse_level_comparison_() {
        if (!advance_()) {
            return std::nullopt;
        }
        if (token_.kind != TokenKind::Compare) {
            return fail_(token_.position, "expected a comparison after 'level'");
        }
        const CompareOp op = token_.op;
        if (!advance_()) {
            return std::nullopt;
        }
        const bool is_word =
            token_.kind == TokenKind::Identifier || token_.kind == TokenKind::Number;
        const std::optional<LogLevel> level = is_word ? parse_level(token_.text) : std::nullopt;
        if (!level) {
            return fail_(token_.position,
                         "expected a level (TRACE, DEBUG, INFO, WARN, ERROR, FATAL)");
        }
        if (!advance_()) {
            return std::nullopt;
        }
        return level_compare(op, *level);
    }

    // key op value
    [[nodiscard]] std::optional<Predicate> parse_tag_comparison_() {
        const std::string key = token_.text;
        if (!advance_()) {
            return std::nullopt;
        }
        if (token_.kind != TokenKind::Compare) {
            return fail_(token_.position, "expected a comparison after '" + key +
                                              "' (for presence use exists(" + key + "))");
        }
        const CompareOp op = token_.op;
        const std::size_t op_position = token_.position;
        if (!advance_()) {
            return std::nullopt;
        }
        if (token_.kind == TokenKind::Number) {
            return parse_number_value_(key, op);
        }
        if (token_.kind == TokenKind::String || token_.kind == TokenKind::Identifier) {
            return parse_text_value_(key, op, op_position);
        }
        return fail_(token_.position, "expected a value after '" + key + "'");
    }

    [[nodiscard]] std::optional<Predicate> parse_number_value_(std::string_view key, CompareOp op) {
        const double value = token_.number;
        if (!advance_()) {
            return std::nullopt;
        }
        return tag_compare(key, op, value);
    }

    [[nodiscard]] std::optional<Predicate> parse_text_value_(std::string_view key, CompareOp op,
                                                             std::size_t op_position) {
        if (op != CompareOp::Equal && op != CompareOp::NotEqual) {
            return fail_(op_position, "'<', '<=', '>' and '>=' need a number on the right");
        }
        const std::string value = token_.text;
        if (!advance_()) {
            return std::nullopt;
        }
        return op == CompareOp::Equal ? tag_equals(key, value) : tag_not_equals(key, value);
    }

    // Makes the next token current; false (with `error_` set) if it is malformed.
    [[nodiscard]] bool advance_() {
        std::expected<Token, PredicateParseError> next = lexer_.next();
        if (!next) {
            error_ = std::move(next.error());
            return false;
        }
        token_ = std::move(*next);
        return true;
    }

    [[nodiscard]] bool is_keyword_or_(TokenKind kind, std::string_view keyword) const {
        return token_.kind == kind ||
               (token_.kind == TokenKind::Identifier && iequals(token_.text, keyword));
    }

    [[nodiscard]] bool expect_(TokenKind kind, std::string_view what) {
        if (token_.kind != kind) {
            error_ = detail::make_parse_error(token_.position, "expected " + std::string(what));
            return false;
        }
        return advance_();
    }

    [[nodiscard]] std::nullopt_t fail_(std::size_t position, std::string message) {
        error_ = detail::make_parse_error(position, std::move(message));
        return std::nullopt;
    }

    PredicateLexer lexer_;
    Token token_;
    PredicateParseError error_;
};

}  // namespace

std::expected<Predicate, PredicateParseError> parse_predicate(std::string_view query) {
    return Parser(query).parse();
}

}  // namespace lumen
