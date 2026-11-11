// The Eigen bridge, compiled with Eigen on the include path and as the only
// include: the guard must let the content through and the header must be
// self-contained. Naming the API fails the build if either is not so.
#if !__has_include(<Eigen/Core>)
#error "this check must be compiled with Eigen on the include path"
#endif

#include "lumen/bridges/eigen.hpp"

static_assert(lumen::bridges::EigenFloatingMatrix<Eigen::MatrixXd>);
static_assert(lumen::bridges::computation_info_name(Eigen::Success) == "success");
