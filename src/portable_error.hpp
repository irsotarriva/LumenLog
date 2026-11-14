#ifndef LUMEN_SRC_PORTABLE_ERROR_HPP
#define LUMEN_SRC_PORTABLE_ERROR_HPP

#include <system_error>

namespace lumen::detail {

// The error code a caller can compare with std::errc on every standard
// library. A failed std::thread constructor throws std::system_error with an
// errno value: libstdc++ in std::generic_category, libc++ in
// std::system_category. A system_category code whose default condition is a
// generic one (every errno value on POSIX) is returned as that generic code;
// any other code is returned unchanged.
[[nodiscard]] inline std::error_code portable_error(std::error_code code) noexcept {
    if (code.category() != std::system_category()) {
        return code;
    }
    const std::error_condition condition = code.default_error_condition();
    if (condition.category() != std::generic_category()) {
        return code;
    }
    return {condition.value(), std::generic_category()};
}

}  // namespace lumen::detail

#endif
