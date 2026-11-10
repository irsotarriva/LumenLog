#ifndef LUMEN_DETAIL_CLASS_NAME_H
#define LUMEN_DETAIL_CLASS_NAME_H

#include <source_location>
#include <string_view>

// ── Tier 2: C++26 static reflection (explicit type required) ───────────────────
//    Enabled when the compiler supports the ^T operator and <meta> header.
//    Use class_name_of<T>() to obtain the name of a known type at compile time.
#ifdef LUMEN_HAS_REFLECTION

#if __has_include(<experimental/meta>)
#include <experimental/meta>
#else
#include <meta>
#endif

namespace lumen {
namespace detail {

template <typename T>
[[nodiscard]] consteval std::string_view class_name_of() noexcept {
    return std::meta::name_of(^T);
}

}  // namespace detail
}  // namespace lumen

#endif  // LUMEN_HAS_REFLECTION

// ── Tier 1: compile-time string parsing of function_name ──────────────────────
//    Works on all C++20+ compilers. Extracts the innermost class name from
//    signatures like "void MyClass::method(args)" or
//    "auto Outer::Inner::operator()(int) -> void".
//    Returns empty string_view for free functions and lambdas without a named
//    enclosing class.
namespace lumen {
namespace detail {

[[nodiscard]] constexpr std::string_view extract_class_name(
    std::string_view function_name) noexcept {
    const auto paren = function_name.rfind('(');
    if (paren == std::string_view::npos) {
        return {};
    }

    const auto method_sep = function_name.rfind("::", paren);
    if (method_sep == std::string_view::npos) {
        return {};
    }

    auto class_start = method_sep;
    while (class_start > 0) {
        const char c = function_name[class_start - 1];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_') {
            --class_start;
        } else {
            break;
        }
    }

    if (class_start == method_sep) {
        return {};
    }

    return function_name.substr(class_start, method_sep - class_start);
}

[[nodiscard]] inline std::string_view class_name(
    std::source_location loc = std::source_location::current()) noexcept {
    return extract_class_name(loc.function_name());
}

}  // namespace detail
}  // namespace lumen

#endif
