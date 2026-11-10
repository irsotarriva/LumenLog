#ifndef LUMEN_SRC_PREDICATE_LEXER_HPP
#define LUMEN_SRC_PREDICATE_LEXER_HPP

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include "lumen/predicate.hpp"

namespace lumen::detail {

enum class TokenKind : uint8_t {
    Identifier,
    Number,
    String,
    Compare,
    And,
    Or,
    Not,
    LParen,
    RParen,
    End,
};

struct Token {
    TokenKind kind = TokenKind::End;
    // Identifier or number spelling, unescaped string contents, operator
    // spelling, or "end of query"; error messages quote it.
    std::string text;
    CompareOp op = CompareOp::Equal;  // Compare only
    double number = 0.0;              // Number only
    size_t position = 0;              // byte offset of the token's first character
};

[[nodiscard]] PredicateParseError make_parse_error(size_t position, std::string message);

// Splits a predicate query into tokens, one per call to next(). After the
// last token, next() returns End tokens for ever.
class PredicateLexer {
public:
    explicit PredicateLexer(std::string_view query) : query_(query) {}

    [[nodiscard]] std::expected<Token, PredicateParseError> next();

private:
    using Result = std::expected<Token, PredicateParseError>;

    void skip_whitespace_();
    [[nodiscard]] char peek_(size_t offset) const;
    [[nodiscard]] Token take_(TokenKind kind, size_t length);
    [[nodiscard]] Result fail_(size_t position, std::string message) const;

    [[nodiscard]] Result lex_end_() const;
    [[nodiscard]] Result lex_paren_();
    [[nodiscard]] Result lex_logical_();
    [[nodiscard]] Result lex_operator_();
    [[nodiscard]] Result lex_string_();
    [[nodiscard]] Result lex_identifier_();
    [[nodiscard]] Result lex_number_();

    std::string_view query_;
    size_t pos_ = 0;
};

}  // namespace lumen::detail

#endif
