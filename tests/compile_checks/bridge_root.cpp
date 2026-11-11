// The ROOT bridge, compiled with ROOT on the include path and as the only
// include: the guard must let the content through and the header must be
// self-contained. Naming the API fails the build if either is not so.
#if !__has_include(<TTree.h>)
#error "this check must be compiled with ROOT on the include path"
#endif

#include "lumen/bridges/root.hpp"

#include <type_traits>

static_assert(
    std::is_invocable_v<decltype(&lumen::bridges::emit_tree_summary), const TTree&, lumen::Core&>);
