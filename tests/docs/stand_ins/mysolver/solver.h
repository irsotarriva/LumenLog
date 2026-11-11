// A made-up third-party library, so that the "writing a bridge" example in
// docs/bridges.md compiles with its __has_include guard satisfied.
#ifndef LUMEN_TESTS_DOCS_STAND_INS_MYSOLVER_SOLVER_H
#define LUMEN_TESTS_DOCS_STAND_INS_MYSOLVER_SOLVER_H

namespace mysolver {

class Solver {
public:
    [[nodiscard]] double residual() const { return 0.0; }
};

}  // namespace mysolver

#endif
