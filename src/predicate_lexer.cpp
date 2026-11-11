#include "predicate_lexer.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "ascii.hpp"
#include "lumen/error.hpp"
#include "number_parse.hpp"

namespace lumen::detail {

namespace {

[[nodiscard]] constexpr bool is_paren(char c) {
    return c == '(' || c == ')';
}

[[nodiscard]] constexpr bool is_logical(char c) {
    return c == '&' || c == '|';
}

[[nodiscard]] constexpr bool is_operator_start(char c) {
    return c == '=' || c == '!' || c == '<' || c == '>';
}

[[nodiscard]] constexpr bool is_quote(char c) {
    return c == '"' || c == '\'';
}

[[nodiscard]] constexpr bool is_identifier_start(char c) {
    return ascii::is_alpha(c) || c == '_';
}

[[nodiscard]] constexpr bool is_identifier_char(char c) {
    return ascii::is_alnum(c) || c == '_' || c == '.';
}

[[nodiscard]] constexpr bool is_number_start(char c) {
    return ascii::is_digit(c) || c == '-' || c == '+' || c == '.';
}

// A number ends at whitespace or at a character that starts another token.
[[nodiscard]] constexpr bool is_delimiter(char c) {
    return ascii::is_space(c) || is_paren(c) || is_logical(c) || is_operator_start(c) ||
           is_quote(c);
}

// `c` is one of = ! < >, and `has_eq` says whether '=' follows it.
[[nodiscard]] constexpr CompareOp comparison_op(char c, bool has_eq) {
    switch (c) {
        case '=': return CompareOp::Equal;
        case '!': return CompareOp::NotEqual;
        case '<': return has_eq ? CompareOp::LessEqual : CompareOp::Less;
        default: return has_eq ? CompareOp::GreaterEqual : CompareOp::Greater;
    }
}

}  // namespace

PredicateParseError make_parse_error(size_t position, std::string message) {
    return PredicateParseError{make_error_code(LumenError::InvalidPredicate), position,
                               std::move(message)};
}

std::expected<Token, PredicateParseError> PredicateLexer::next() {
    skip_whitespace_();
    if (pos_ >= query_.size()) {
        return lex_end_();
    }
    const char c = query_[pos_];
    if (is_paren(c)) {
        return lex_paren_();
    }
    if (is_logical(c)) {
        return lex_logical_();
    }
    if (is_operator_start(c)) {
        return lex_operator_();
    }
    if (is_quote(c)) {
        return lex_string_();
    }
    if (is_identifier_start(c)) {
        return lex_identifier_();
    }
    if (is_number_start(c)) {
        return lex_number_();
    }
    return fail_(pos_, std::string("unexpected character '") + c + "'");
}

void PredicateLexer::skip_whitespace_() {
    while (pos_ < query_.size() && ascii::is_space(query_[pos_])) {
        ++pos_;
    }
}

char PredicateLexer::peek_(size_t offset) const {
    return pos_ + offset < query_.size() ? query_[pos_ + offset] : '\0';
}

// The token is the next `length` characters, spelled as they are.
Token PredicateLexer::take_(TokenKind kind, size_t length) {
    Token token{.kind = kind, .text = std::string(query_.substr(pos_, length)), .position = pos_};
    pos_ += length;
    return token;
}

PredicateLexer::Result PredicateLexer::fail_(size_t position, std::string message) const {
    return std::unexpected(make_parse_error(position, std::move(message)));
}

PredicateLexer::Result PredicateLexer::lex_end_() const {
    return Token{.kind = TokenKind::End, .text = "end of query", .position = pos_};
}

PredicateLexer::Result PredicateLexer::lex_paren_() {
    return take_(peek_(0) == '(' ? TokenKind::LParen : TokenKind::RParen, 1);
}

PredicateLexer::Result PredicateLexer::lex_logical_() {
    const char c = peek_(0);
    if (peek_(1) != c) {
        return fail_(pos_, std::string("expected '") + c + c + "'");
    }
    return take_(c == '&' ? TokenKind::And : TokenKind::Or, 2);
}

PredicateLexer::Result PredicateLexer::lex_operator_() {
    const char c = peek_(0);
    const bool has_eq = peek_(1) == '=';
    if (c == '!' && !has_eq) {
        return take_(TokenKind::Not, 1);
    }
    if (c == '=' && !has_eq) {
        return fail_(pos_, "expected '==' (single '=' is not an operator)");
    }
    Token token = take_(TokenKind::Compare, has_eq ? 2 : 1);
    token.op = comparison_op(c, has_eq);
    return token;
}

PredicateLexer::Result PredicateLexer::lex_string_() {
    const char quote = peek_(0);
    const size_t start = pos_;
    ++pos_;
    std::string out;
    while (pos_ < query_.size() && query_[pos_] != quote) {
        // A backslash makes the next character literal; one at the very end
        // is kept as itself and the string is then unterminated.
        if (query_[pos_] == '\\' && pos_ + 1 < query_.size()) {
            ++pos_;
        }
        out += query_[pos_];
        ++pos_;
    }
    if (pos_ >= query_.size()) {
        return fail_(start, "unterminated string");
    }
    ++pos_;  // closing quote
    return Token{.kind = TokenKind::String, .text = std::move(out), .position = start};
}

PredicateLexer::Result PredicateLexer::lex_identifier_() {
    size_t length = 0;
    while (is_identifier_char(peek_(length))) {
        ++length;
    }
    return take_(TokenKind::Identifier, length);
}

PredicateLexer::Result PredicateLexer::lex_number_() {
    size_t length = 0;
    while (pos_ + length < query_.size() && !is_delimiter(query_[pos_ + length])) {
        ++length;
    }
    const std::string_view text = query_.substr(pos_, length);
    // Tag values never carry a leading '+', but a query may: skip it here.
    const std::string_view digits = text.starts_with('+') ? text.substr(1) : text;
    const std::optional<double> value = parse_double(digits);
    if (!value) {
        return fail_(pos_, "invalid number '" + std::string(text) + "' (quote it to compare as text)");
    }
    Token token = take_(TokenKind::Number, length);
    token.number = *value;
    return token;
}

}  // namespace lumen::detail
