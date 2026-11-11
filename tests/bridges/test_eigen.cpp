// lumen/bridges/eigen.hpp against the real Eigen: the exact metrics and tags
// that reach a sink.
#include <gtest/gtest.h>

#include <Eigen/Core>
#include <Eigen/IterativeLinearSolvers>
#include <cmath>
#include <complex>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "lumen/bridges/eigen.hpp"
#include "lumen/macros.hpp"
#include "test_support.hpp"

namespace lumen {
namespace {

using bridges::emit_matrix_summary;
using bridges::emit_solver_summary;
using test::tags_of;
using Tags = std::vector<std::pair<std::string, std::string>>;

class EigenBridgeTest : public test::LocalCoreTest {
protected:
    [[nodiscard]] std::vector<MetricRecord> _metrics() {
        _core.flush();
        return _captured->metrics();
    }
};

static_assert(bridges::EigenFloatingMatrix<Eigen::MatrixXd>);
static_assert(bridges::EigenFloatingMatrix<Eigen::Matrix3f>);
static_assert(bridges::EigenFloatingMatrix<Eigen::MatrixXcd>);
static_assert(!bridges::EigenFloatingMatrix<Eigen::MatrixXi>, "integer matrices are rejected");
static_assert(!bridges::EigenFloatingMatrix<Eigen::ArrayXXd>, "arrays are not matrices");
static_assert(bridges::EigenIterativeSolver<Eigen::ConjugateGradient<Eigen::MatrixXd>>);
static_assert(!bridges::EigenIterativeSolver<Eigen::MatrixXd>);

TEST_F(EigenBridgeTest, MatrixSummaryEmitsTheFrobeniusNormThenTheNonFiniteCountWithShapeTags) {
    Eigen::MatrixXd m(2, 2);
    m << 3.0, 4.0, 0.0, 0.0;
    emit_matrix_summary("weights", m, _core);

    const auto metrics = _metrics();
    ASSERT_EQ(metrics.size(), 2U);
    const Tags expected_tags = {{"matrix", "weights"}, {"rows", "2"}, {"cols", "2"}};
    EXPECT_EQ(metrics[0].name, "eigen.matrix.norm");
    EXPECT_DOUBLE_EQ(metrics[0].value, 5.0);
    EXPECT_EQ(tags_of(metrics[0].tags), expected_tags);
    EXPECT_EQ(metrics[1].name, "eigen.matrix.non_finite");
    EXPECT_EQ(metrics[1].value, 0.0);
    EXPECT_EQ(tags_of(metrics[1].tags), expected_tags);
}

TEST_F(EigenBridgeTest, MatrixSummaryCountsNaNAndInfiniteCoefficientsAndTheNormIsNaN) {
    constexpr double nan = std::numeric_limits<double>::quiet_NaN();
    constexpr double inf = std::numeric_limits<double>::infinity();
    Eigen::Matrix2d m;
    m << nan, 1.0, inf, -inf;
    emit_matrix_summary("bad", m, _core);

    const auto metrics = _metrics();
    ASSERT_EQ(metrics.size(), 2U);
    EXPECT_TRUE(std::isnan(metrics[0].value));
    EXPECT_EQ(metrics[1].value, 3.0);
}

TEST_F(EigenBridgeTest, MatrixSummaryOfAnInfiniteButNotNaNMatrixHasAnInfiniteNorm) {
    Eigen::Matrix2d m;
    m << std::numeric_limits<double>::infinity(), 0.0, 0.0, 1.0;
    emit_matrix_summary("overflowed", m, _core);

    const auto metrics = _metrics();
    ASSERT_EQ(metrics.size(), 2U);
    EXPECT_TRUE(std::isinf(metrics[0].value));
    EXPECT_EQ(metrics[1].value, 1.0);
}

TEST_F(EigenBridgeTest, MatrixSummaryNormOfHugeFiniteCoefficientsIsFinite) {
    // The squares overflow a double; the norm, 2e300, does not.
    const Eigen::MatrixXd m = Eigen::MatrixXd::Constant(2, 2, 1e300);
    emit_matrix_summary("huge", m, _core);

    const auto metrics = _metrics();
    ASSERT_EQ(metrics.size(), 2U);
    EXPECT_DOUBLE_EQ(metrics[0].value, 2e300);
    EXPECT_EQ(metrics[1].value, 0.0);
}

TEST_F(EigenBridgeTest, MatrixSummaryNormOfTinyNonzeroCoefficientsIsNonzero) {
    // The squares underflow to 0; the norm, 2e-300, does not.
    const Eigen::MatrixXd m = Eigen::MatrixXd::Constant(2, 2, 1e-300);
    emit_matrix_summary("tiny", m, _core);

    const auto metrics = _metrics();
    ASSERT_EQ(metrics.size(), 2U);
    EXPECT_DOUBLE_EQ(metrics[0].value, 2e-300);
}

TEST_F(EigenBridgeTest,
       MatrixSummaryNormBeyondTheLargestDoubleIsInfiniteWithNoNonFiniteCoefficient) {
    constexpr double max = std::numeric_limits<double>::max();
    Eigen::Vector2d v(max, max);
    emit_matrix_summary("beyond", v, _core);

    const auto metrics = _metrics();
    ASSERT_EQ(metrics.size(), 2U);
    EXPECT_TRUE(std::isinf(metrics[0].value));
    EXPECT_EQ(metrics[1].value, 0.0);
}

TEST_F(EigenBridgeTest, MatrixSummaryOfAnExpressionDescribesItsValue) {
    Eigen::RowVector2d a;
    a << 3.0, 4.0;
    emit_matrix_summary("doubled", a + a, _core);

    const auto metrics = _metrics();
    ASSERT_EQ(metrics.size(), 2U);
    EXPECT_DOUBLE_EQ(metrics[0].value, 10.0);
    EXPECT_EQ(tags_of(metrics[0].tags),
              (Tags{{"matrix", "doubled"}, {"rows", "1"}, {"cols", "2"}}));
}

TEST_F(EigenBridgeTest, MatrixSummaryOfAVectorUsesItsEuclideanNorm) {
    const Eigen::Vector3d v(1.0, 2.0, 2.0);
    emit_matrix_summary("v", v, _core);

    const auto metrics = _metrics();
    ASSERT_EQ(metrics.size(), 2U);
    EXPECT_DOUBLE_EQ(metrics[0].value, 3.0);
    EXPECT_EQ(test::tag_of(metrics[0].tags, "rows"), "3");
    EXPECT_EQ(test::tag_of(metrics[0].tags, "cols"), "1");
}

TEST_F(EigenBridgeTest, MatrixSummaryOfAComplexMatrixUsesTheModulusOfEachCoefficient) {
    Eigen::MatrixXcd m(1, 2);
    m << std::complex<double>(3.0, 4.0), std::complex<double>(0.0, 0.0);
    emit_matrix_summary("amplitudes", m, _core);

    const auto metrics = _metrics();
    ASSERT_EQ(metrics.size(), 2U);
    EXPECT_DOUBLE_EQ(metrics[0].value, 5.0);
    EXPECT_EQ(metrics[1].value, 0.0);
}

TEST_F(EigenBridgeTest, MatrixSummaryOfASinglePrecisionMatrixIsEmittedAsDouble) {
    Eigen::Matrix2f m;
    m << 1.5F, 1.5F, 1.5F, 1.5F;
    emit_matrix_summary("f", m, _core);

    const auto metrics = _metrics();
    ASSERT_EQ(metrics.size(), 2U);
    EXPECT_DOUBLE_EQ(metrics[0].value, 3.0);
}

TEST_F(EigenBridgeTest, MatrixSummaryOfAnEmptyMatrixIsZeroWithZeroShape) {
    const Eigen::MatrixXd empty(0, 0);
    emit_matrix_summary("empty", empty, _core);

    const auto metrics = _metrics();
    ASSERT_EQ(metrics.size(), 2U);
    EXPECT_EQ(metrics[0].value, 0.0);
    EXPECT_EQ(metrics[1].value, 0.0);
    EXPECT_EQ(tags_of(metrics[0].tags), (Tags{{"matrix", "empty"}, {"rows", "0"}, {"cols", "0"}}));
}

// A symmetric positive-definite system that is not diagonal, so that the
// solver's default (diagonal) preconditioner does not solve it at once.
// Dynamic-size types: Eigen's iterative solvers do not accept fixed-size ones.
struct SpdSystem {
    Eigen::MatrixXd a = Eigen::MatrixXd(3, 3);
    Eigen::VectorXd b = Eigen::VectorXd(3);
    SpdSystem() {
        a << 4.0, 1.0, 0.0, 1.0, 3.0, 1.0, 0.0, 1.0, 2.0;
        b << 1.0, 2.0, 3.0;
    }
};

TEST_F(EigenBridgeTest, SolverSummaryAfterAConvergedSolveEmitsIterationsAndErrorTaggedSuccess) {
    const SpdSystem system;
    Eigen::ConjugateGradient<Eigen::MatrixXd, Eigen::Lower | Eigen::Upper> solver;
    solver.compute(system.a);
    const Eigen::VectorXd x = solver.solve(system.b);
    ASSERT_EQ(solver.info(), Eigen::Success);
    ASSERT_TRUE((system.a * x).isApprox(system.b, 1e-10));

    emit_solver_summary("cg", solver, _core);

    const auto metrics = _metrics();
    ASSERT_EQ(metrics.size(), 2U);
    const Tags expected_tags = {{"solver", "cg"}, {"info", "success"}};
    EXPECT_EQ(metrics[0].name, "eigen.solver.iterations");
    EXPECT_EQ(metrics[0].value, static_cast<double>(solver.iterations()));
    EXPECT_GE(metrics[0].value, 1.0);
    EXPECT_LE(metrics[0].value, 3.0);  // CG needs at most n iterations for an n x n system
    EXPECT_EQ(tags_of(metrics[0].tags), expected_tags);
    EXPECT_EQ(metrics[1].name, "eigen.solver.error");
    EXPECT_EQ(metrics[1].value, solver.error());
    EXPECT_LT(metrics[1].value, solver.tolerance());
    EXPECT_EQ(tags_of(metrics[1].tags), expected_tags);
}

TEST_F(EigenBridgeTest, SolverSummaryAfterHittingTheIterationLimitIsTaggedNoConvergence) {
    const SpdSystem system;
    Eigen::ConjugateGradient<Eigen::MatrixXd, Eigen::Lower | Eigen::Upper> solver;
    solver.setMaxIterations(1);
    solver.compute(system.a);
    static_cast<void>(solver.solve(system.b).eval());  // only the solver's report matters
    ASSERT_EQ(solver.info(), Eigen::NoConvergence);

    emit_solver_summary("cg_capped", solver, _core);

    const auto metrics = _metrics();
    ASSERT_EQ(metrics.size(), 2U);
    EXPECT_EQ(metrics[0].value, 1.0);
    EXPECT_EQ(test::tag_of(metrics[0].tags, "info"), "no_convergence");
    EXPECT_EQ(metrics[1].value, solver.error());
    EXPECT_GT(metrics[1].value, solver.tolerance());
    EXPECT_EQ(test::tag_of(metrics[1].tags, "info"), "no_convergence");
}

TEST(EigenBridgeComputationInfo, EachValueHasItsOwnName) {
    EXPECT_EQ(bridges::computation_info_name(Eigen::Success), "success");
    EXPECT_EQ(bridges::computation_info_name(Eigen::NumericalIssue), "numerical_issue");
    EXPECT_EQ(bridges::computation_info_name(Eigen::NoConvergence), "no_convergence");
    EXPECT_EQ(bridges::computation_info_name(Eigen::InvalidInput), "invalid_input");
}

TEST_F(EigenBridgeTest, RecordsCarryTheAmbientScopeAfterTheirOwnTags) {
    LUMEN_SCOPE("run", "7");
    emit_matrix_summary("m", Eigen::Matrix2d::Identity(), _core);

    const auto metrics = _metrics();
    ASSERT_EQ(metrics.size(), 2U);
    EXPECT_EQ(tags_of(metrics[0].tags),
              (Tags{{"matrix", "m"}, {"rows", "2"}, {"cols", "2"}, {"run", "7"}}));
}

class EigenBridgeGlobalCoreTest : public test::GlobalCoreTest {};

TEST_F(EigenBridgeGlobalCoreTest, WithoutACoreArgumentTheRecordsGoToTheGlobalCore) {
    emit_matrix_summary("global", Eigen::Matrix2d::Identity());
    core().flush();

    const auto metrics = _captured->metrics();
    ASSERT_EQ(metrics.size(), 2U);
    EXPECT_EQ(metrics[0].name, "eigen.matrix.norm");
    EXPECT_DOUBLE_EQ(metrics[0].value, std::sqrt(2.0));
}

}  // namespace
}  // namespace lumen
