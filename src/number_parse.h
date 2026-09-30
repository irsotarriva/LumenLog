#ifndef LUMEN_SRC_NUMBER_PARSE_H
#define LUMEN_SRC_NUMBER_PARSE_H

#include <charconv>
#include <optional>
#include <string_view>

#if !defined(__cpp_lib_to_chars)
#include <locale>
#include <sstream>
#include <string>
#endif

namespace lumen::detail {

// Parses `text` as a double; the whole string must be a number ("42abc" and ""
// are not). Independent of the global locale.
[[nodiscard]] inline std::optional<double> parse_double(std::string_view text) {
    // from_chars rejects leading whitespace and '+'; the fallback must too.
    if (text.empty() || text.front() == '+' || text.front() == ' ' || text.front() == '\t' ||
        text.front() == '\n') {
        return std::nullopt;
    }
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

}  // namespace lumen::detail

#endif
