// lumen/diagnostics.hpp alone declares what it documents.
#include "lumen/diagnostics.hpp"

#include <string_view>
#include <type_traits>

namespace lumen {

static_assert(std::is_same_v<decltype(stderr_diagnostics()), DiagnosticHandler>);
static_assert(std::is_invocable_r_v<void, DiagnosticHandler, std::string_view>);

}  // namespace lumen
