#ifndef _OPENMP
#error "this check must be compiled with OpenMP enabled"
#endif

#include "openmp.hpp"

int main() {
    lumen::bridges::openmp_emit_thread_metrics();
    lumen::core().flush();
    return 0;
}
