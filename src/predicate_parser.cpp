#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "lumen/detail/error.h"
#include "lumen/predicate.h"

namespace lumen {

namespace {

// Deep enough for any hand-written query; stops "((((..." from a console
// exhausting the stack.
constexpr uint32_t MAX_NESTING = 64;

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
    std::string text;  // identifier/number spelling, or unescaped string contents
    CompareOp op = CompareOp::EQ;
    double number = 0.0;
    size_t position = 0;
};

[[nodiscard]] bool is_identifier_start(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
}

[[nodiscard]] bool is_identifier_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '.';
}

// A number ends at whitespace or at a character that starts another token.
[[nodiscard]] bool is_delimiter(char c) {
    return std::isspace(static_cast<unsigned char>(c)) != 0 || c == '(' || c == ')' ||
           c == '!' || c == '=' || c == '<' || c == '>' || c == '&' || c == '|' ||
           c == '"' || c == '\'';
}

[[nodiscard]] bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::optional<LogLevel> parse_level(std::string_view text) {
    constexpr std::string_view NAMES[] = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL"};
    for (uint32_t i = 0; i < std::size(NAMES); ++i) {
        if (iequals(text, NAMES[i])) return static_cast<LogLevel>(i);
    }
    if (text.size() == 1 && text[0] >= '0' && text[0] <= '5') {
        return static_cast<LogLevel>(text[0] - '0');
    }
    return std::nullopt;
}

class Parser {
public:
    explicit Parser(std::string_view query) : __query(query) {}

    [[nodiscard]] std::expected<Predicate, PredicateParseError> parse() {
        if (!__advance()) return std::unexpected(std::move(__error));
        std::optional<Predicate> result = __parse_or(0);
        if (!result) return std::unexpected(std::move(__error));
        if (__token.kind != TokenKind::End) {
            return std::unexpected(__make_error(__token.position, "unexpected '" + __token.text + "'"));
        }
        return std::move(*result);
    }

private:
    // ── Lexer ────────────────────────────────────────────────────────────────

    [[nodiscard]] bool __advance() {
        while (__pos < __query.size() && std::isspace(static_cast<unsigned char>(__query[__pos])) != 0) {
            ++__pos;
        }
        __token = Token{};
        __token.position = __pos;
        if (__pos >= __query.size()) {
            __token.kind = TokenKind::End;
            __token.text = "end of query";
            return true;
        }

        const char c = __query[__pos];
        const char next = __pos + 1 < __query.size() ? __query[__pos + 1] : '\0';

        if (c == '(' || c == ')') {
            __token.kind = c == '(' ? TokenKind::LParen : TokenKind::RParen;
            __token.text = std::string(1, c);
            ++__pos;
            return true;
        }
        if (c == '&' || c == '|') {
            if (next != c) {
                __error = __make_error(__pos, std::string("expected '") + c + c + "'");
                return false;
            }
            __token.kind = c == '&' ? TokenKind::And : TokenKind::Or;
            __token.text = std::string(2, c);
            __pos += 2;
            return true;
        }
        if (c == '=' || c == '!' || c == '<' || c == '>') {
            return __lex_operator(c, next);
        }
        if (c == '"' || c == '\'') {
            return __lex_string(c);
        }
        if (is_identifier_start(c)) {
            const size_t start = __pos;
            while (__pos < __query.size() && is_identifier_char(__query[__pos])) ++__pos;
            __token.kind = TokenKind::Identifier;
            __token.text = std::string(__query.substr(start, __pos - start));
            return true;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '+' || c == '.') {
            return __lex_number();
        }
        __error = __make_error(__pos, std::string("unexpected character '") + c + "'");
        return false;
    }

    [[nodiscard]] bool __lex_operator(char c, char next) {
        const bool has_eq = next == '=';
        __token.kind = TokenKind::Compare;
        if (c == '=') {
            if (!has_eq) {
                __error = __make_error(__pos, "expected '==' (single '=' is not an operator)");
                return false;
            }
            __token.op = CompareOp::EQ;
        } else if (c == '!') {
            if (!has_eq) {
                __token.kind = TokenKind::Not;
                __token.text = "!";
                ++__pos;
                return true;
            }
            __token.op = CompareOp::NE;
        } else if (c == '<') {
            __token.op = has_eq ? CompareOp::LE : CompareOp::LT;
        } else {
            __token.op = has_eq ? CompareOp::GE : CompareOp::GT;
        }
        const size_t len = has_eq ? 2 : 1;
        __token.text = std::string(__query.substr(__pos, len));
        __pos += len;
        return true;
    }

    [[nodiscard]] bool __lex_string(char quote) {
        const size_t start = __pos;
        ++__pos;
        std::string out;
        while (__pos < __query.size() && __query[__pos] != quote) {
            char ch = __query[__pos];
            if (ch == '\\' && __pos + 1 < __query.size()) {
                ch = __query[++__pos];
            }
            out += ch;
            ++__pos;
        }
        if (__pos >= __query.size()) {
            __error = __make_error(start, "unterminated string");
            return false;
        }
        ++__pos;  // closing quote
        __token.kind = TokenKind::String;
        __token.text = std::move(out);
        return true;
    }

    [[nodiscard]] bool __lex_number() {
        const size_t start = __pos;
        while (__pos < __query.size() && !is_delimiter(__query[__pos])) ++__pos;
        const std::string_view text = __query.substr(start, __pos - start);
        // from_chars rejects a leading '+', so skip it ourselves.
        const std::string_view digits = !text.empty() && text[0] == '+' ? text.substr(1) : text;
        double value = 0.0;
        const char* const end = digits.data() + digits.size();
        const auto [ptr, ec] = std::from_chars(digits.data(), end, value);
        if (digits.empty() || ec != std::errc{} || ptr != end) {
            __error = __make_error(start, "invalid number '" + std::string(text) +
                                              "' (quote it to compare as text)");
            return false;
        }
        __token.kind = TokenKind::Number;
        __token.text = std::string(text);
        __token.number = value;
        return true;
    }

    // ── Grammar ──────────────────────────────────────────────────────────────

    [[nodiscard]] std::optional<Predicate> __parse_or(uint32_t depth) {
        std::optional<Predicate> lhs = __parse_and(depth);
        while (lhs && __is_keyword_or(TokenKind::Or, "or")) {
            if (!__advance()) return std::nullopt;
            std::optional<Predicate> rhs = __parse_and(depth);
            if (!rhs) return std::nullopt;
            lhs = *lhs || *rhs;
        }
        return lhs;
    }

    [[nodiscard]] std::optional<Predicate> __parse_and(uint32_t depth) {
        std::optional<Predicate> lhs = __parse_unary(depth);
        while (lhs && __is_keyword_or(TokenKind::And, "and")) {
            if (!__advance()) return std::nullopt;
            std::optional<Predicate> rhs = __parse_unary(depth);
            if (!rhs) return std::nullopt;
            lhs = *lhs && *rhs;
        }
        return lhs;
    }

    [[nodiscard]] std::optional<Predicate> __parse_unary(uint32_t depth) {
        if (depth >= MAX_NESTING) {
            __error = __make_error(__token.position, "query nested too deeply");
            return std::nullopt;
        }
        if (__is_keyword_or(TokenKind::Not, "not")) {
            if (!__advance()) return std::nullopt;
            std::optional<Predicate> operand = __parse_unary(depth + 1);
            if (!operand) return std::nullopt;
            return !*operand;
        }
        return __parse_primary(depth);
    }

    [[nodiscard]] std::optional<Predicate> __parse_primary(uint32_t depth) {
        if (__token.kind == TokenKind::LParen) {
            if (!__advance()) return std::nullopt;
            std::optional<Predicate> inner = __parse_or(depth + 1);
            if (!inner) return std::nullopt;
            if (!__expect(TokenKind::RParen, "')'")) return std::nullopt;
            return inner;
        }

        if (__token.kind == TokenKind::Identifier) {
            if (iequals(__token.text, "true") || iequals(__token.text, "false")) {
                const bool value = iequals(__token.text, "true");
                if (!__advance()) return std::nullopt;
                return value ? always() : never();
            }
            if (iequals(__token.text, "exists")) {
                return __parse_exists();
            }
            if (iequals(__token.text, "level")) {
                return __parse_level_comparison();
            }
            return __parse_tag_comparison();
        }

        if (__token.kind == TokenKind::String) {
            return __parse_tag_comparison();  // quoted key, e.g. "level" == 3
        }

        if (__token.kind == TokenKind::End) {
            __error = __make_error(__token.position, "unexpected end of query");
        } else {
            __error = __make_error(__token.position, "expected a condition, got '" + __token.text + "'");
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<Predicate> __parse_exists() {
        if (!__advance()) return std::nullopt;
        if (!__expect(TokenKind::LParen, "'(' after exists")) return std::nullopt;
        if (__token.kind != TokenKind::Identifier && __token.kind != TokenKind::String) {
            __error = __make_error(__token.position, "expected a tag name in exists(...)");
            return std::nullopt;
        }
        std::string key = __token.text;
        if (!__advance()) return std::nullopt;
        if (!__expect(TokenKind::RParen, "')'")) return std::nullopt;
        return tag_exists(key);
    }

    [[nodiscard]] std::optional<Predicate> __parse_level_comparison() {
        if (!__advance()) return std::nullopt;
        if (__token.kind != TokenKind::Compare) {
            __error = __make_error(__token.position, "expected a comparison after 'level'");
            return std::nullopt;
        }
        const CompareOp op = __token.op;
        if (!__advance()) return std::nullopt;
        const std::optional<LogLevel> level =
            __token.kind == TokenKind::Identifier || __token.kind == TokenKind::Number
                ? parse_level(__token.text)
                : std::nullopt;
        if (!level) {
            __error = __make_error(__token.position,
                                   "expected a level (TRACE, DEBUG, INFO, WARN, ERROR, FATAL)");
            return std::nullopt;
        }
        if (!__advance()) return std::nullopt;
        return level_compare(op, *level);
    }

    [[nodiscard]] std::optional<Predicate> __parse_tag_comparison() {
        std::string key = __token.text;
        if (!__advance()) return std::nullopt;
        if (__token.kind != TokenKind::Compare) {
            __error = __make_error(__token.position,
                                   "expected a comparison after '" + key + "' (for presence use exists(" +
                                       key + "))");
            return std::nullopt;
        }
        const CompareOp op = __token.op;
        const size_t op_position = __token.position;
        if (!__advance()) return std::nullopt;

        if (__token.kind == TokenKind::Number) {
            const double value = __token.number;
            if (!__advance()) return std::nullopt;
            return tag_compare(key, op, value);
        }
        if (__token.kind == TokenKind::String || __token.kind == TokenKind::Identifier) {
            if (op != CompareOp::EQ && op != CompareOp::NE) {
                __error = __make_error(op_position, "'<', '<=', '>' and '>=' need a number on the right");
                return std::nullopt;
            }
            std::string value = __token.text;
            if (!__advance()) return std::nullopt;
            if (op == CompareOp::EQ) return tag_equals(key, value);
            return tag_exists(key) && !tag_equals(key, value);
        }
        __error = __make_error(__token.position, "expected a value after '" + key + "'");
        return std::nullopt;
    }

    // ── Helpers ──────────────────────────────────────────────────────────────

    [[nodiscard]] bool __is_keyword_or(TokenKind kind, std::string_view keyword) const {
        return __token.kind == kind ||
               (__token.kind == TokenKind::Identifier && iequals(__token.text, keyword));
    }

    [[nodiscard]] bool __expect(TokenKind kind, std::string_view what) {
        if (__token.kind != kind) {
            __error = __make_error(__token.position, "expected " + std::string(what));
            return false;
        }
        return __advance();
    }

    [[nodiscard]] static PredicateParseError __make_error(size_t position, std::string message) {
        return PredicateParseError{make_error_code(LumenError::invalid_predicate), position,
                                   std::move(message)};
    }

    std::string_view __query;
    size_t __pos = 0;
    Token __token;
    PredicateParseError __error;
};

}  // namespace

std::expected<Predicate, PredicateParseError> parse_predicate(std::string_view query) {
    return Parser(query).parse();
}

}  // namespace lumen
