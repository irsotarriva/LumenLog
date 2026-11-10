#ifndef LUMEN_DETAIL_CLASS_NAME_HPP
#define LUMEN_DETAIL_CLASS_NAME_HPP

#include <string_view>

namespace lumen::detail {

// The innermost class named in a function signature as produced by
// std::source_location::function_name(), for example "MyClass" from
// "void MyClass::method(int)" or "Inner" from "auto Outer::Inner::f() -> void".
// Returns an empty view for a free function. It only reads the text, so a
// namespace-qualified free function ("void ns::f()") yields the namespace.
[[nodiscard]] constexpr std::string_view extract_class_name(
    std::string_view function_name) noexcept {
    const auto paren = function_name.rfind('(');
    if (paren == std::string_view::npos) return {};

    const auto method_sep = function_name.rfind("::", paren);
    if (method_sep == std::string_view::npos) return {};

    auto class_start = method_sep;
    while (class_start > 0) {
        const char c = function_name[class_start - 1];
        const bool identifier_char =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (!identifier_char) break;
        --class_start;
    }
    return function_name.substr(class_start, method_sep - class_start);
}

}  // namespace lumen::detail

#endif
