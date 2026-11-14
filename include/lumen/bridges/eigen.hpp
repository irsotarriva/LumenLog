#ifndef LUMEN_BRIDGES_EIGEN_HPP
#define LUMEN_BRIDGES_EIGEN_HPP

// Eigen helpers. Nothing here hooks Eigen: each function emits records only
// when the program calls it, typically after a step whose result it wants to
// watch.
//
//   emit_matrix_summary(name, m)     size, Frobenius norm and non-finite
//                                    coefficients of a matrix or vector
//   emit_solver_summary(name, s)     iterations, error estimate and outcome of
//                                    an iterative solver after solve()
//
// The header is empty unless <Eigen/Core> can be included.

#if __has_include(<Eigen/Core>)

#include <Eigen/Core>

#include <concepts>
#include <cstdint>
#include <string_view>

#include "lumen/core.hpp"
#include "lumen/record_builder.hpp"

namespace lumen::bridges {

// A dense matrix, vector or matrix expression with floating-point or complex
// coefficients (integer matrices have no meaningful norm here).
template <typename Derived>
concept EigenFloatingMatrix = std::derived_from<Derived, Eigen::MatrixBase<Derived>> &&
                              !Eigen::NumTraits<typename Derived::Scalar>::IsInteger;

// What iterative solvers (ConjugateGradient, BiCGSTAB, LeastSquaresConjugateGradient,
// and solvers built on Eigen::IterativeSolverBase) report about their last solve().
template <typename Solver>
concept EigenIterativeSolver = requires(const Solver& solver) {
    { solver.iterations() } -> std::convertible_to<Eigen::Index>;
    { solver.error() } -> std::convertible_to<double>;
    { solver.info() } -> std::same_as<Eigen::ComputationInfo>;
};

// The text of an Eigen::ComputationInfo, as emit_solver_summary tags it.
[[nodiscard]] constexpr std::string_view computation_info_name(Eigen::ComputationInfo info) {
    switch (info) {
        case Eigen::Success:        return "success";
        case Eigen::NumericalIssue: return "numerical_issue";
        case Eigen::NoConvergence:  return "no_convergence";
        case Eigen::InvalidInput:   return "invalid_input";
    }
    return "unknown";
}

// Emits two metrics, each tagged matrix=<name>, rows=<rows>, cols=<cols>:
//   eigen.matrix.norm        the Frobenius norm (for a vector, its Euclidean
//                            norm), computed with Blue's scaled algorithm
//                            (blueNorm()), so it neither overflows nor
//                            underflows when the coefficients are finite and
//                            the norm itself is representable: 1e300 or
//                            1e-300 coefficients give a finite, nonzero norm.
//                            NaN if a coefficient is NaN; otherwise infinite
//                            if a coefficient is infinite or the norm exceeds
//                            the largest finite value of the scalar type
//   eigen.matrix.non_finite  how many coefficients are NaN or infinite
// An expression (a * b, m.transpose()) is evaluated once, into a temporary.
template <EigenFloatingMatrix Derived>
void emit_matrix_summary(std::string_view name, const Eigen::MatrixBase<Derived>& matrix,
                         Core& core = lumen::core()) {
    // A reference to `matrix` itself when it is a plain matrix, otherwise a
    // temporary holding the evaluated expression (lifetime-extended).
    const auto& evaluated = matrix.eval();
    const auto rows = static_cast<int64_t>(evaluated.rows());
    const auto cols = static_cast<int64_t>(evaluated.cols());
    const auto non_finite = static_cast<int64_t>((!evaluated.array().isFinite()).count());
    // Not stableNorm(): in Eigen 3.4.0 it fails an assertion (and reads out
    // of bounds without one) on a fixed-size matrix that is not a vector.
    MetricBuilder(core, "eigen.matrix.norm", static_cast<double>(evaluated.blueNorm()))
        .tag("matrix", name)
        .tag("rows", rows)
        .tag("cols", cols);
    MetricBuilder(core, "eigen.matrix.non_finite", static_cast<double>(non_finite))
        .tag("matrix", name)
        .tag("rows", rows)
        .tag("cols", cols);
}

// Emits two metrics, each tagged solver=<name> and
// info=success|numerical_issue|no_convergence|invalid_input:
//   eigen.solver.iterations  solver.iterations() of the last solve()
//   eigen.solver.error       solver.error(): Eigen's estimate of the relative
//                            residual |Ax - b| / |b| of the last solve()
// Precondition: solve() has been called. Before compute(), Eigen asserts in
// these accessors; between compute() and solve() the values are not defined.
template <EigenIterativeSolver Solver>
void emit_solver_summary(std::string_view name, const Solver& solver, Core& core = lumen::core()) {
    const std::string_view info = computation_info_name(solver.info());
    MetricBuilder(core, "eigen.solver.iterations", static_cast<double>(solver.iterations()))
        .tag("solver", name)
        .tag("info", info);
    MetricBuilder(core, "eigen.solver.error", static_cast<double>(solver.error()))
        .tag("solver", name)
        .tag("info", info);
}

}  // namespace lumen::bridges

#endif  // __has_include(<Eigen/Core>)

#endif
