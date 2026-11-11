#ifndef LUMEN_SRC_NUMBER_PARSE_HPP
#define LUMEN_SRC_NUMBER_PARSE_HPP

#include <charconv>
#include <cmath>
#include <optional>
#include <string_view>

#if !defined(__cpp_lib_to_chars)
#include <locale>
#include <sstream>
#include <string>
#endif

#include "ascii.hpp"

namespace lumen::detail {

// Converts all of `text` with the platform's number parser. Accepts what
// std::from_chars accepts, except that the stream fallback also skips leading
// whitespace and a leading '+': parse_double rules those out first.
[[nodiscard]] inline std::optional<double> parse_whole_double(std::string_view text) {
    double value = 0.0;
#if defined(__cpp_lib_to_chars)
    const char* const end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value);
    if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
#else
    // libc++ (including Apple's) does not provide floating-point from_chars
    // yet. A stream imbued with the classic locale is the portable fallback.
    std::istringstream in{std::string(text)};
    in.imbue(std::locale::classic());
    in >> value;
    if (in.fail() || in.peek() != std::char_traits<char>::eof()) {
        return std::nullopt;
    }
#endif
    return value;
}

// Parses `text` as a finite double; the whole string must be a number
// ("42abc", "", " 1" and "+1" are not). Non-finite spellings ("nan",
// "nan(...)", "inf", "infinity", with or without '-') and values out of the
// double range ("1e999") are not numbers either. Independent of the global
// locale.
[[nodiscard]] inline std::optional<double> parse_double(std::string_view text) {
    if (text.empty() || text.front() == '+' || ascii::is_space(text.front())) {
        return std::nullopt;
    }
    const std::optional<double> value = parse_whole_double(text);
    if (!value || !std::isfinite(*value)) {
        return std::nullopt;
    }
    return value;
}

}  // namespace lumen::detail

#endif
