// The libtorch bridge, compiled with libtorch on the include path and as the
// only include: the guard must let the content through and the header must be
// self-contained. Naming the API fails the build if either is not so.
#if !__has_include(<torch/torch.h>)
#error "this check must be compiled with libtorch on the include path"
#endif

#include "lumen/bridges/torch.hpp"

#include <system_error>
#include <type_traits>

static_assert(std::is_destructible_v<lumen::bridges::TorchTrainingMetrics>);
static_assert(std::is_error_code_enum_v<lumen::bridges::TorchMetricsError>);
