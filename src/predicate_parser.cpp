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
    explicit Parser(std::string_view query) : query_(query) {}

    [[nodiscard]] std::expected<Predicate, PredicateParseError> parse() {
        if (!advance_()) return std::unexpected(std::move(error_));
        std::optional<Predicate> result = parse_or_(0);
        if (!result) return std::unexpected(std::move(error_));
        if (token_.kind != TokenKind::End) {
            return std::unexpected(make_error_(token_.position, "unexpected '" + token_.text + "'"));
        }
        return std::move(*result);
    }

private:
    // ── Lexer ────────────────────────────────────────────────────────────────

    [[nodiscard]] bool advance_() {
        while (pos_ < query_.size() && std::isspace(static_cast<unsigned char>(query_[pos_])) != 0) {
            ++pos_;
        }
        token_ = Token{};
        token_.position = pos_;
        if (pos_ >= query_.size()) {
            token_.kind = TokenKind::End;
            token_.text = "end of query";
            return true;
        }

        const char c = query_[pos_];
        const char next = pos_ + 1 < query_.size() ? query_[pos_ + 1] : '\0';

        if (c == '(' || c == ')') {
            token_.kind = c == '(' ? TokenKind::LParen : TokenKind::RParen;
            token_.text = std::string(1, c);
            ++pos_;
            return true;
        }
        if (c == '&' || c == '|') {
            if (next != c) {
                error_ = make_error_(pos_, std::string("expected '") + c + c + "'");
                return false;
            }
            token_.kind = c == '&' ? TokenKind::And : TokenKind::Or;
            token_.text = std::string(2, c);
            pos_ += 2;
            return true;
        }
        if (c == '=' || c == '!' || c == '<' || c == '>') {
            return lex_operator_(c, next);
        }
        if (c == '"' || c == '\'') {
            return lex_string_(c);
        }
        if (is_identifier_start(c)) {
            const size_t start = pos_;
            while (pos_ < query_.size() && is_identifier_char(query_[pos_])) ++pos_;
            token_.kind = TokenKind::Identifier;
            token_.text = std::string(query_.substr(start, pos_ - start));
            return true;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '+' || c == '.') {
            return lex_number_();
        }
        error_ = make_error_(pos_, std::string("unexpected character '") + c + "'");
        return false;
    }

    [[nodiscard]] bool lex_operator_(char c, char next) {
        const bool has_eq = next == '=';
        token_.kind = TokenKind::Compare;
        if (c == '=') {
            if (!has_eq) {
                error_ = make_error_(pos_, "expected '==' (single '=' is not an operator)");
                return false;
            }
            token_.op = CompareOp::EQ;
        } else if (c == '!') {
            if (!has_eq) {
                token_.kind = TokenKind::Not;
                token_.text = "!";
                ++pos_;
                return true;
            }
            token_.op = CompareOp::NE;
        } else if (c == '<') {
            token_.op = has_eq ? CompareOp::LE : CompareOp::LT;
        } else {
            token_.op = has_eq ? CompareOp::GE : CompareOp::GT;
        }
        const size_t len = has_eq ? 2 : 1;
        token_.text = std::string(query_.substr(pos_, len));
        pos_ += len;
        return true;
    }

    [[nodiscard]] bool lex_string_(char quote) {
        const size_t start = pos_;
        ++pos_;
        std::string out;
        while (pos_ < query_.size() && query_[pos_] != quote) {
            char ch = query_[pos_];
            if (ch == '\\' && pos_ + 1 < query_.size()) {
                ch = query_[++pos_];
            }
            out += ch;
            ++pos_;
        }
        if (pos_ >= query_.size()) {
            error_ = make_error_(start, "unterminated string");
            return false;
        }
        ++pos_;  // closing quote
        token_.kind = TokenKind::String;
        token_.text = std::move(out);
        return true;
    }

    [[nodiscard]] bool lex_number_() {
        const size_t start = pos_;
        while (pos_ < query_.size() && !is_delimiter(query_[pos_])) ++pos_;
        const std::string_view text = query_.substr(start, pos_ - start);
        // from_chars rejects a leading '+', so skip it ourselves.
        const std::string_view digits = !text.empty() && text[0] == '+' ? text.substr(1) : text;
        double value = 0.0;
        const char* const end = digits.data() + digits.size();
        const auto [ptr, ec] = std::from_chars(digits.data(), end, value);
        if (digits.empty() || ec != std::errc{} || ptr != end) {
            error_ = make_error_(start, "invalid number '" + std::string(text) +
                                              "' (quote it to compare as text)");
            return false;
        }
        token_.kind = TokenKind::Number;
        token_.text = std::string(text);
        token_.number = value;
        return true;
    }

    // ── Grammar ──────────────────────────────────────────────────────────────

    [[nodiscard]] std::optional<Predicate> parse_or_(uint32_t depth) {
        std::optional<Predicate> lhs = parse_and_(depth);
        while (lhs && is_keyword_or_(TokenKind::Or, "or")) {
            if (!advance_()) return std::nullopt;
            std::optional<Predicate> rhs = parse_and_(depth);
            if (!rhs) return std::nullopt;
            lhs = *lhs || *rhs;
        }
        return lhs;
    }

    [[nodiscard]] std::optional<Predicate> parse_and_(uint32_t depth) {
        std::optional<Predicate> lhs = parse_unary_(depth);
        while (lhs && is_keyword_or_(TokenKind::And, "and")) {
            if (!advance_()) return std::nullopt;
            std::optional<Predicate> rhs = parse_unary_(depth);
            if (!rhs) return std::nullopt;
            lhs = *lhs && *rhs;
        }
        return lhs;
    }

    [[nodiscard]] std::optional<Predicate> parse_unary_(uint32_t depth) {
        if (depth >= MAX_NESTING) {
            error_ = make_error_(token_.position, "query nested too deeply");
            return std::nullopt;
        }
        if (is_keyword_or_(TokenKind::Not, "not")) {
            if (!advance_()) return std::nullopt;
            std::optional<Predicate> operand = parse_unary_(depth + 1);
            if (!operand) return std::nullopt;
            return !*operand;
        }
        return parse_primary_(depth);
    }

    [[nodiscard]] std::optional<Predicate> parse_primary_(uint32_t depth) {
        if (token_.kind == TokenKind::LParen) {
            if (!advance_()) return std::nullopt;
            std::optional<Predicate> inner = parse_or_(depth + 1);
            if (!inner) return std::nullopt;
            if (!expect_(TokenKind::RParen, "')'")) return std::nullopt;
            return inner;
        }

        if (token_.kind == TokenKind::Identifier) {
            if (iequals(token_.text, "true") || iequals(token_.text, "false")) {
                const bool value = iequals(token_.text, "true");
                if (!advance_()) return std::nullopt;
                return value ? always() : never();
            }
            if (iequals(token_.text, "exists")) {
                return parse_exists_();
            }
            if (iequals(token_.text, "level")) {
                return parse_level_comparison_();
            }
            return parse_tag_comparison_();
        }

        if (token_.kind == TokenKind::String) {
            return parse_tag_comparison_();  // quoted key, e.g. "level" == 3
        }

        if (token_.kind == TokenKind::End) {
            error_ = make_error_(token_.position, "unexpected end of query");
        } else {
            error_ = make_error_(token_.position, "expected a condition, got '" + token_.text + "'");
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<Predicate> parse_exists_() {
        if (!advance_()) return std::nullopt;
        if (!expect_(TokenKind::LParen, "'(' after exists")) return std::nullopt;
        if (token_.kind != TokenKind::Identifier && token_.kind != TokenKind::String) {
            error_ = make_error_(token_.position, "expected a tag name in exists(...)");
            return std::nullopt;
        }
        std::string key = token_.text;
        if (!advance_()) return std::nullopt;
        if (!expect_(TokenKind::RParen, "')'")) return std::nullopt;
        return tag_exists(key);
    }

    [[nodiscard]] std::optional<Predicate> parse_level_comparison_() {
        if (!advance_()) return std::nullopt;
        if (token_.kind != TokenKind::Compare) {
            error_ = make_error_(token_.position, "expected a comparison after 'level'");
            return std::nullopt;
        }
        const CompareOp op = token_.op;
        if (!advance_()) return std::nullopt;
        const std::optional<LogLevel> level =
            token_.kind == TokenKind::Identifier || token_.kind == TokenKind::Number
                ? parse_level(token_.text)
                : std::nullopt;
        if (!level) {
            error_ = make_error_(token_.position,
                                   "expected a level (TRACE, DEBUG, INFO, WARN, ERROR, FATAL)");
            return std::nullopt;
        }
        if (!advance_()) return std::nullopt;
        return level_compare(op, *level);
    }

    [[nodiscard]] std::optional<Predicate> parse_tag_comparison_() {
        std::string key = token_.text;
        if (!advance_()) return std::nullopt;
        if (token_.kind != TokenKind::Compare) {
            error_ = make_error_(token_.position,
                                   "expected a comparison after '" + key + "' (for presence use exists(" +
                                       key + "))");
            return std::nullopt;
        }
        const CompareOp op = token_.op;
        const size_t op_position = token_.position;
        if (!advance_()) return std::nullopt;

        if (token_.kind == TokenKind::Number) {
            const double value = token_.number;
            if (!advance_()) return std::nullopt;
            return tag_compare(key, op, value);
        }
        if (token_.kind == TokenKind::String || token_.kind == TokenKind::Identifier) {
            if (op != CompareOp::EQ && op != CompareOp::NE) {
                error_ = make_error_(op_position, "'<', '<=', '>' and '>=' need a number on the right");
                return std::nullopt;
            }
            std::string value = token_.text;
            if (!advance_()) return std::nullopt;
            if (op == CompareOp::EQ) return tag_equals(key, value);
            return tag_exists(key) && !tag_equals(key, value);
        }
        error_ = make_error_(token_.position, "expected a value after '" + key + "'");
        return std::nullopt;
    }

    // ── Helpers ──────────────────────────────────────────────────────────────

    [[nodiscard]] bool is_keyword_or_(TokenKind kind, std::string_view keyword) const {
        return token_.kind == kind ||
               (token_.kind == TokenKind::Identifier && iequals(token_.text, keyword));
    }

    [[nodiscard]] bool expect_(TokenKind kind, std::string_view what) {
        if (token_.kind != kind) {
            error_ = make_error_(token_.position, "expected " + std::string(what));
            return false;
        }
        return advance_();
    }

    [[nodiscard]] static PredicateParseError make_error_(size_t position, std::string message) {
        return PredicateParseError{make_error_code(LumenError::invalid_predicate), position,
                                   std::move(message)};
    }

    std::string_view query_;
    size_t pos_ = 0;
    Token token_;
    PredicateParseError error_;
};

}  // namespace

std::expected<Predicate, PredicateParseError> parse_predicate(std::string_view query) {
    return Parser(query).parse();
}

}  // namespace lumen
