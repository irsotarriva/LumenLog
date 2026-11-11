// The OpenMP bridge, compiled with OpenMP 3.0 or later enabled and as the only include:
// the guard must let the content through and the header must be
// self-contained. Naming the API fails the build if either is not so.
#if !defined(_OPENMP) || _OPENMP < 200805
#error "this check must be compiled with OpenMP 3.0 or later enabled"
#endif

#include "lumen/bridges/openmp.hpp"

#include <type_traits>

static_assert(
    std::is_invocable_v<decltype(&lumen::bridges::emit_openmp_configuration), lumen::Core&>);
static_assert(std::is_destructible_v<lumen::bridges::OpenMpWorkBalance>);
