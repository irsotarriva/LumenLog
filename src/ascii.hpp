#ifndef LUMEN_SRC_ASCII_HPP
#define LUMEN_SRC_ASCII_HPP

// Character classes for the predicate query language and number parsing.
// <cctype> answers according to the global C locale, so the same query could
// parse differently in two programs; these never depend on it and match the
// "C" locale.

namespace lumen::detail::ascii {

[[nodiscard]] constexpr bool is_alpha(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

[[nodiscard]] constexpr bool is_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

[[nodiscard]] constexpr bool is_alnum(char c) noexcept {
    return is_alpha(c) || is_digit(c);
}

// The six characters std::isspace accepts in the "C" locale.
[[nodiscard]] constexpr bool is_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

[[nodiscard]] constexpr char to_lower(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

}  // namespace lumen::detail::ascii

#endif
