#ifndef LUMEN_DETAIL_ERROR_H
#define LUMEN_DETAIL_ERROR_H

#include <string>
#include <system_error>

namespace lumen {

enum class LumenError : int {
    none = 0,
    buffer_full,
    invalid_sink_id,
    io_error,
    file_open_error,
    sink_exception,
    invalid_predicate,
};

class LumenErrorCategory : public std::error_category {
public:
    [[nodiscard]] const char* name() const noexcept override;
    [[nodiscard]] std::string message(int ev) const override;
    [[nodiscard]] bool equivalent(const std::error_code& code,
                                  int condition) const noexcept override;
};

[[nodiscard]] const LumenErrorCategory& lumen_error_category();
[[nodiscard]] std::error_code make_error_code(LumenError e);

}  // namespace lumen

namespace std {

template <>
struct is_error_code_enum<lumen::LumenError> : true_type {};

}  // namespace std

#endif
