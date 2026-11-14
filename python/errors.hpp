#ifndef LUMEN_PYTHON_ERRORS_HPP
#define LUMEN_PYTHON_ERRORS_HPP

#include <pybind11/pybind11.h>

#include <concepts>
#include <expected>
#include <string_view>
#include <system_error>
#include <utility>

// How the module turns the std::error_code values the library returns into
// Python exceptions. The exception classes are created by register_errors()
// (python/registration.hpp), which documents the hierarchy.
//
// pybind11 raises a Python exception when a bound function exits with a C++
// exception, so these functions throw; that is the only place this module
// throws, and pybind11 catches every such exception at the binding boundary.

namespace lumen::python {

// Raises the Python exception that stands for `code`. Never returns.
[[noreturn]] void raise_error(std::error_code code);

// As above, with `context` (what failed, e.g. a path) after the code's message.
[[noreturn]] void raise_error(std::error_code code, std::string_view context);

// The value, or the Python exception for the error.
template <std::move_constructible T>
[[nodiscard]] T value_or_raise(std::expected<T, std::error_code> result) {
    if (!result) {
        raise_error(result.error());
    }
    return std::move(*result);
}

inline void value_or_raise(const std::expected<void, std::error_code>& result) {
    if (!result) {
        raise_error(result.error());
    }
}

}  // namespace lumen::python

#endif
