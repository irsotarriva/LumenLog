#ifndef LUMEN_DETAIL_CLASS_NAME_HPP
#define LUMEN_DETAIL_CLASS_NAME_HPP

#include <cstddef>
#include <cstdint>
#include <string_view>

// The class named in a function signature, as compilers spell
// std::source_location::function_name():
//
//   GCC    void Detector::b(std::function<void(int)>)
//          void Detector::c(T) [with T = int]
//          Detector::d()::<lambda()>
//   Clang  void Detector::b(std::function<void (int)>)
//          auto Detector::d()::(anonymous class)::operator()() const
//   MSVC   void __cdecl Detector::b(class std::function<void __cdecl(int)>)
//          auto __cdecl Detector::d::<lambda_1>::operator ()(void) const
//
// The text is read, not parsed, from the end, so that the common case costs
// a few dozen character comparisons: the function's own parameter list is
// the last '(' outside any brackets; the name before it is split at "::"
// outside brackets; its last part is the function, and the nearest part
// before that which names a class is the answer. A lambda counts as part of
// the function that encloses it, so each line above gives "Detector".

namespace lumen::detail {

// ASCII letters, digits and '_': the characters of a C++ identifier as
// compilers spell them in function signatures.
[[nodiscard]] constexpr bool is_class_name_char(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

namespace class_name {

// How the bracket depth changes when a backward scan passes s[i]: closing
// brackets open a level, opening ones close it. "->" is not a bracket.
[[nodiscard]] constexpr int32_t depth_change_backward(std::string_view s, std::size_t i) noexcept {
    switch (s[i]) {
        case ')':
        case ']':
        case '}': return 1;
        case '>': return i > 0 && s[i - 1] == '-' ? 0 : 1;
        case '(':
        case '[':
        case '{':
        case '<': return -1;
        default:  return 0;
    }
}

// The last index at or before `end` (exclusive) where `s` holds `c` outside
// any brackets that close before `end`, or npos. `c` is a punctuation
// character, never one of is_class_name_char().
[[nodiscard]] constexpr std::size_t last_top_level(std::string_view s, std::size_t end,
                                                   char c) noexcept {
    int32_t depth = 0;
    for (std::size_t i = end; i-- > 0;) {
        if (is_class_name_char(s[i])) {
            continue;  // most characters: neither a bracket nor `c`
        }
        const int32_t change = depth_change_backward(s, i);
        // An opening bracket is at the level outside it.
        if (change < 0) {
            depth += change;
        }
        if (depth == 0 && s[i] == c) {
            return i;
        }
        if (change > 0) {
            depth += change;
        }
    }
    return std::string_view::npos;
}

// `qualified` without a trailing operator name ("Foo::operator<<",
// "Foo::operator ()", "Foo::operator int"), whose symbol would look like
// brackets. The "::" before it is kept.
[[nodiscard]] constexpr std::string_view without_operator_name(
    std::string_view qualified) noexcept {
    constexpr std::string_view keyword = "operator";
    const std::size_t at = qualified.rfind(keyword);
    const std::size_t after = at + keyword.size();
    if (at == std::string_view::npos || (at > 0 && is_class_name_char(qualified[at - 1])) ||
        (after < qualified.size() && is_class_name_char(qualified[after]))) {
        return qualified;
    }
    return qualified.substr(0, at);
}

// A scope without a name of its own: a lambda's closure ("<lambda()>",
// "(anonymous class)", "<lambda_1>") or an anonymous namespace
// ("{anonymous}", "(anonymous namespace)", "`anonymous namespace'").
[[nodiscard]] constexpr bool is_unnamed(std::string_view part) noexcept {
    return !part.empty() && std::string_view("<({`").find(part.front()) != std::string_view::npos;
}

// The class name in one part: after any return type or calling convention
// (the last space outside brackets), before any template arguments. Empty if
// that is not an identifier.
[[nodiscard]] constexpr std::string_view class_in(std::string_view part) noexcept {
    const std::size_t space = last_top_level(part, part.size(), ' ');
    std::string_view name = space == std::string_view::npos ? part : part.substr(space + 1);
    name = name.substr(0, name.find('<'));
    for (const char c : name) {
        if (!is_class_name_char(c)) {
            return {};
        }
    }
    return name;
}

}  // namespace class_name

// The innermost class named in `function_name` (see the top of this file),
// for example "MyClass" from "void MyClass::method(int)". Returns an empty
// view for a free function and for text without a parameter list. It only
// reads the text, so a namespace-qualified free function ("void ns::f()")
// yields the namespace.
[[nodiscard]] constexpr std::string_view extract_class_name(
    std::string_view function_name) noexcept {
    using class_name::last_top_level;
    const std::size_t paren = last_top_level(function_name, function_name.size(), '(');
    if (paren == std::string_view::npos) {
        return {};
    }
    const std::string_view qualified =
        class_name::without_operator_name(function_name.substr(0, paren));
    // Walk the parts from the end. The first one is the function itself.
    std::size_t end = last_top_level(qualified, qualified.size(), ':');
    bool skip_enclosing_function = false;
    while (end != std::string_view::npos && end > 0 && qualified[end - 1] == ':') {
        const std::size_t sep = last_top_level(qualified, end - 1, ':');
        const std::size_t begin = sep == std::string_view::npos ? 0 : sep + 1;
        const std::string_view part = qualified.substr(begin, end - 1 - begin);
        if (class_name::is_unnamed(part)) {
            skip_enclosing_function = true;  // the part before a closure is its function
        } else if (skip_enclosing_function) {
            skip_enclosing_function = false;
        } else if (part.find('(') == std::string_view::npos) {
            return class_name::class_in(part);
        }
        end = sep;
    }
    return {};
}

}  // namespace lumen::detail

#endif
