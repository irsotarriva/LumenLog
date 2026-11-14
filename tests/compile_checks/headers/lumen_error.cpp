// lumen/error.hpp alone declares what it documents.
#include "lumen/error.hpp"

#include <array>
#include <type_traits>

namespace lumen {

static_assert(std::is_error_code_enum_v<LumenError>);
static_assert(std::is_same_v<decltype(make_error_code(LumenError::TagTableFull)), std::error_code>);
static_assert(std::is_same_v<decltype(lumen_error_category()), const LumenErrorCategory&>);
static_assert(std::is_base_of_v<std::error_category, LumenErrorCategory>);
inline constexpr std::array every_code{
    LumenError::None,           LumenError::InvalidSinkId,     LumenError::NullSink,
    LumenError::SinkBusy,       LumenError::TagTableFull,      LumenError::InvalidPredicate,
    LumenError::FileOpenFailed, LumenError::InvalidSinkConfig, LumenError::IoError,
    LumenError::NotATerminal,
};

}  // namespace lumen
