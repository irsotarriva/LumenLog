#ifndef LUMEN_ERROR_HPP
#define LUMEN_ERROR_HPP

#include <cstdint>
#include <string>
#include <system_error>

namespace lumen {

// The errors Lumen returns, as std::error_code values in the "lumen"
// category. Compare a returned code with the enumerator directly:
//
//   if (result.error() == lumen::LumenError::TagTableFull) { ... }
enum class LumenError : int32_t {
    None = 0,
    // remove_sink, sink_exception_count: no sink is registered under this id.
    InvalidSinkId,
    // add_sink: the sink pointer is null.
    NullSink,
    // remove_sink: called from inside a callback of the sink being removed.
    SinkBusy,
    // set_process_tag, set_thread_tag, Tagged::lumen_tag: the key is new and
    // the table already holds its maximum number of keys (lumen/limits.hpp).
    TagTableFull,
    // parse_predicate: the query is not valid.
    InvalidPredicate,
};

class LumenErrorCategory : public std::error_category {
public:
    [[nodiscard]] const char* name() const noexcept override;
    [[nodiscard]] std::string message(int ev) const override;
};

[[nodiscard]] const LumenErrorCategory& lumen_error_category();
[[nodiscard]] std::error_code make_error_code(LumenError e);

}  // namespace lumen

template <>
struct std::is_error_code_enum<lumen::LumenError> : std::true_type {};

#endif
