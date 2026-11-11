// PredicateLexer (src/predicate_lexer.hpp): the tokens of a query, their
// positions, and the lexical errors. The parser's own tests are in
// test_predicate_parser.cpp.
#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "lumen/error.hpp"
#include "predicate_lexer.hpp"

namespace lumen {
namespace {

using detail::PredicateLexer;
using detail::Token;
using detail::TokenKind;

// Every token of `query` up to and including End; fails the test on an error.
std::vector<Token> tokens_of(std::string_view query) {
    PredicateLexer lexer(query);
    std::vector<Token> out;
    for (std::size_t guard = 0; guard <= query.size() + 1; ++guard) {
        auto token = lexer.next();
        EXPECT_TRUE(token.has_value()) << "query: " << query << ": " << token.error().message;
        if (!token) {
            break;
        }
        out.push_back(*token);
        if (out.back().kind == TokenKind::End) {
            break;
        }
    }
    return out;
}

// The error of the first token that fails.
PredicateParseError first_error(std::string_view query) {
    PredicateLexer lexer(query);
    for (std::size_t guard = 0; guard <= query.size() + 1; ++guard) {
        auto token = lexer.next();
        if (!token) {
            return token.error();
        }
        if (token->kind == TokenKind::End) {
            break;
        }
    }
    ADD_FAILURE() << "no lexical error in: " << query;
    return {};
}

std::vector<TokenKind> kinds_of(const std::vector<Token>& tokens) {
    std::vector<TokenKind> out;
    for (const Token& t : tokens) {
        out.push_back(t.kind);
    }
    return out;
}

TEST(PredicateLexerTest, SplitsAQueryIntoKindsWithTheirSpellingAndPosition) {
    constexpr std::string_view query = "level>=WARN && (run == 'a b' || !exists(x))";
    const auto tokens = tokens_of(query);
    EXPECT_EQ(kinds_of(tokens),
              (std::vector<TokenKind>{
                  TokenKind::Identifier, TokenKind::Compare, TokenKind::Identifier, TokenKind::And,
                  TokenKind::LParen, TokenKind::Identifier, TokenKind::Compare, TokenKind::String,
                  TokenKind::Or, TokenKind::Not, TokenKind::Identifier, TokenKind::LParen,
                  TokenKind::Identifier, TokenKind::RParen, TokenKind::RParen, TokenKind::End}));
    ASSERT_EQ(tokens.size(), 16U);
    EXPECT_EQ(tokens[1].text, ">=");
    EXPECT_EQ(tokens[1].op, CompareOp::GreaterEqual);
    EXPECT_EQ(tokens[1].position, 5U);
    EXPECT_EQ(tokens[7].text, "a b");
    EXPECT_EQ(tokens[7].position, 23U);
    EXPECT_EQ(tokens[15].position, query.size());
}

TEST(PredicateLexerTest, EachComparisonSpellingGivesItsOperator) {
    const auto tokens = tokens_of("== != < <= > >=");
    ASSERT_EQ(tokens.size(), 7U);
    EXPECT_EQ(tokens[0].op, CompareOp::Equal);
    EXPECT_EQ(tokens[1].op, CompareOp::NotEqual);
    EXPECT_EQ(tokens[2].op, CompareOp::Less);
    EXPECT_EQ(tokens[3].op, CompareOp::LessEqual);
    EXPECT_EQ(tokens[4].op, CompareOp::Greater);
    EXPECT_EQ(tokens[5].op, CompareOp::GreaterEqual);
}

TEST(PredicateLexerTest, NumbersCarryTheirValueAndALeadingPlusIsAccepted) {
    const auto tokens = tokens_of("42 -3.5 1e-6 +7 .5");
    ASSERT_EQ(tokens.size(), 6U);
    EXPECT_EQ(tokens[0].number, 42.0);
    EXPECT_EQ(tokens[1].number, -3.5);
    EXPECT_EQ(tokens[2].number, 1e-6);
    EXPECT_EQ(tokens[3].number, 7.0);
    EXPECT_EQ(tokens[3].text, "+7");
    EXPECT_EQ(tokens[4].number, 0.5);
    for (std::size_t i = 0; i < 5; ++i) {
        EXPECT_EQ(tokens[i].kind, TokenKind::Number) << i;
    }
}

TEST(PredicateLexerTest, NonFiniteOrOutOfRangeNumbersAreErrors) {
    for (const std::string_view query : {"-inf", "-nan", "-infinity", "+inf", "1e999", "-1e999"}) {
        const PredicateParseError err = first_error(query);
        EXPECT_EQ(err.code, LumenError::InvalidPredicate) << query;
        EXPECT_EQ(err.position, 0U) << query;
        EXPECT_NE(err.message.find("invalid number"), std::string::npos) << err.message;
    }
}

TEST(PredicateLexerTest, InfAndNanWithoutASignAreIdentifiers) {
    const auto tokens = tokens_of("inf nan");
    ASSERT_EQ(tokens.size(), 3U);
    EXPECT_EQ(tokens[0].kind, TokenKind::Identifier);
    EXPECT_EQ(tokens[0].text, "inf");
    EXPECT_EQ(tokens[1].kind, TokenKind::Identifier);
}

TEST(PredicateLexerTest, ABackslashMakesTheNextCharacterLiteralInAString) {
    const auto tokens = tokens_of(R"("say \"hi\"" 'it\'s')");
    ASSERT_EQ(tokens.size(), 3U);
    EXPECT_EQ(tokens[0].text, R"(say "hi")");
    EXPECT_EQ(tokens[1].text, "it's");
}

TEST(PredicateLexerTest, AfterTheLastTokenEndIsReturnedForEver) {
    PredicateLexer lexer("x");
    ASSERT_TRUE(lexer.next().has_value());
    for (int i = 0; i < 3; ++i) {
        const auto token = lexer.next();
        ASSERT_TRUE(token.has_value());
        EXPECT_EQ(token->kind, TokenKind::End);
        EXPECT_EQ(token->position, 1U);
    }
}

TEST(PredicateLexerTest, LexicalErrorsGiveTheirPositionAndMessage) {
    struct Case {
        std::string_view query;
        std::size_t position;
        std::string_view message_part;
    };
    for (const Case& c : {Case{"a & b", 2, "expected '&&'"}, Case{"a | b", 2, "expected '||'"},
                          Case{"a = 1", 2, "single '='"}, Case{"a == 'open", 5, "unterminated"},
                          Case{"a == 1 # x", 7, "unexpected character '#'"},
                          Case{"a == 42abc", 5, "invalid number '42abc'"}}) {
        const PredicateParseError err = first_error(c.query);
        EXPECT_EQ(err.code, LumenError::InvalidPredicate) << c.query;
        EXPECT_EQ(err.position, c.position) << c.query;
        EXPECT_NE(err.message.find(c.message_part), std::string::npos)
            << c.query << ": " << err.message;
    }
}

}  // namespace
}  // namespace lumen
