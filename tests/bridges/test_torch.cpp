// lumen/bridges/torch.hpp against the real libtorch: the exact metrics and
// tags that reach a sink, and the errors returned instead.
#include <gtest/gtest.h>

#include <torch/torch.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "lumen/bridges/torch.hpp"
#include "lumen/macros.hpp"
#include "test_support.hpp"

namespace lumen {
namespace {

using bridges::TorchMetricsError;
using bridges::TorchTrainingMetrics;
using test::tags_of;
using Tags = std::vector<std::pair<std::string, std::string>>;

class TorchBridgeTest : public test::LocalCoreTest {
protected:
    [[nodiscard]] std::vector<MetricRecord> _metrics() {
        _core.flush();
        return _captured->metrics();
    }
};

TEST_F(TorchBridgeTest, LossOfASingleValueTensorIsEmittedTaggedWithTheModel) {
    const TorchTrainingMetrics metrics("classifier", _core);
    EXPECT_TRUE(metrics.emit_loss(torch::tensor(0.25)).has_value());
    EXPECT_TRUE(metrics.emit_loss(torch::ones({1, 1}) * 0.5).has_value());

    const auto records = _metrics();
    ASSERT_EQ(records.size(), 2U);
    EXPECT_EQ(records[0].name, "torch.loss");
    EXPECT_EQ(records[0].value, 0.25);
    EXPECT_EQ(tags_of(records[0].tags), (Tags{{"model", "classifier"}}));
    EXPECT_EQ(records[1].value, 0.5);
}

TEST_F(TorchBridgeTest, LossOfATensorWithSeveralElementsIsNotAScalarAndEmitsNothing) {
    const TorchTrainingMetrics metrics("classifier", _core);
    const auto result = metrics.emit_loss(torch::ones({2}));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), TorchMetricsError::NotAScalar);
    EXPECT_TRUE(_metrics().empty());
}

TEST_F(TorchBridgeTest, LossOfAnUndefinedTensorIsNotAScalarAndEmitsNothing) {
    const TorchTrainingMetrics metrics("classifier", _core);
    const auto result = metrics.emit_loss(torch::Tensor());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), TorchMetricsError::NotAScalar);
    EXPECT_TRUE(_metrics().empty());
}

TEST_F(TorchBridgeTest, ALibtorchExceptionIsReturnedAsFrameworkExceptionAndLoggedAsWarn) {
    const TorchTrainingMetrics metrics("classifier", _core);
    // A meta tensor has a shape but no data, so item() throws.
    const torch::Tensor meta =
        torch::empty({1}, torch::TensorOptions().device(c10::Device(c10::DeviceType::Meta)));
    const auto result = metrics.emit_loss(meta);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), TorchMetricsError::FrameworkException);

    _core.flush();
    EXPECT_TRUE(_captured->metrics().empty());
    const auto logs = _captured->logs();
    ASSERT_EQ(logs.size(), 1U);
    EXPECT_EQ(logs[0].level, LogLevel::Warn);
    EXPECT_TRUE(logs[0].message.starts_with("libtorch: ")) << logs[0].message;
    EXPECT_GT(logs[0].message.size(), std::string_view("libtorch: ").size());
    EXPECT_EQ(test::tag_of(logs[0].tags, "model"), "classifier");
}

// For y = w . x + b summed, dL/dw = x and dL/db = 1, whatever w and b are.
TEST_F(TorchBridgeTest, GradientNormIsTheGlobalL2NormOverAllParameters) {
    torch::nn::Linear linear(2, 1);
    linear->forward(torch::tensor({{1.0, 2.0}})).sum().backward();

    const TorchTrainingMetrics metrics("linear", _core);
    const auto norm = metrics.emit_gradient_norm(*linear);
    ASSERT_TRUE(norm.has_value());
    // sqrt(1^2 + 2^2 + 1^2); the sum of the per-tensor norms would be sqrt(5) + 1.
    EXPECT_NEAR(*norm, std::sqrt(6.0), 1e-6);

    const auto records = _metrics();
    ASSERT_EQ(records.size(), 1U);
    EXPECT_EQ(records[0].name, "torch.grad_norm");
    EXPECT_EQ(records[0].value, *norm);
    EXPECT_EQ(tags_of(records[0].tags), (Tags{{"model", "linear"}}));
}

TEST_F(TorchBridgeTest, GradientNormLeavesOutParametersWithoutAGradient) {
    torch::nn::Linear linear(2, 1);
    linear->bias.set_requires_grad(false);
    linear->forward(torch::tensor({{1.0, 2.0}})).sum().backward();

    const TorchTrainingMetrics metrics("linear", _core);
    const auto norm = metrics.emit_gradient_norm(*linear);
    ASSERT_TRUE(norm.has_value());
    EXPECT_NEAR(*norm, std::sqrt(5.0), 1e-6);
}

TEST_F(TorchBridgeTest, GradientNormIncludesTheParametersOfSubmodules) {
    torch::nn::Sequential model(torch::nn::Linear(3, 4), torch::nn::Tanh(),
                                torch::nn::Linear(4, 1));
    model->forward(torch::ones({5, 3})).pow(2).sum().backward();

    double expected_squares = 0.0;
    std::size_t parameters = 0;
    for (const torch::Tensor& p : model->parameters()) {
        expected_squares += p.grad().pow(2).sum().item<double>();
        ++parameters;
    }
    ASSERT_EQ(parameters, 4U);  // two weights, two biases

    const TorchTrainingMetrics metrics("mlp", _core);
    const auto norm = metrics.emit_gradient_norm(*model);
    ASSERT_TRUE(norm.has_value());
    EXPECT_NEAR(*norm, std::sqrt(expected_squares), 1e-5 * std::sqrt(expected_squares));
}

TEST_F(TorchBridgeTest, GradientNormBeforeBackwardIsNoGradientsAndEmitsNothing) {
    const torch::nn::Linear linear(2, 1);
    const TorchTrainingMetrics metrics("linear", _core);
    const auto norm = metrics.emit_gradient_norm(*linear);
    ASSERT_FALSE(norm.has_value());
    EXPECT_EQ(norm.error(), TorchMetricsError::NoGradients);
    EXPECT_TRUE(_metrics().empty());
}

TEST_F(TorchBridgeTest, LearningRateIsReadFromEachParameterGroupOfTheOptimizer) {
    torch::nn::Linear first(2, 2);
    torch::nn::Linear second(2, 1);
    torch::optim::SGD sgd(first->parameters(), torch::optim::SGDOptions(0.1));
    sgd.add_param_group(torch::optim::OptimizerParamGroup(second->parameters()));
    sgd.param_groups()[1].options().set_lr(0.01);  // as a scheduler would

    const TorchTrainingMetrics metrics("pair", _core);
    EXPECT_TRUE(metrics.emit_learning_rates(sgd).has_value());

    const auto records = _metrics();
    ASSERT_EQ(records.size(), 2U);
    EXPECT_EQ(records[0].name, "torch.learning_rate");
    EXPECT_EQ(records[0].value, 0.1);
    EXPECT_EQ(tags_of(records[0].tags), (Tags{{"model", "pair"}, {"group", "0"}}));
    EXPECT_EQ(records[1].name, "torch.learning_rate");
    EXPECT_EQ(records[1].value, 0.01);
    EXPECT_EQ(tags_of(records[1].tags), (Tags{{"model", "pair"}, {"group", "1"}}));
}

TEST_F(TorchBridgeTest, LearningRateOfAdamIsItsConfiguredRate) {
    torch::nn::Linear linear(2, 1);
    torch::optim::Adam adam(linear->parameters(), torch::optim::AdamOptions(1e-3));

    const TorchTrainingMetrics metrics("adam", _core);
    EXPECT_TRUE(metrics.emit_learning_rates(adam).has_value());

    const auto records = _metrics();
    ASSERT_EQ(records.size(), 1U);
    EXPECT_EQ(records[0].value, 1e-3);
}

TEST_F(TorchBridgeTest, RecordsCarryTheAmbientScopeAfterTheModelTag) {
    const TorchTrainingMetrics metrics("classifier", _core);
    LUMEN_SCOPE("epoch", "3");
    EXPECT_TRUE(metrics.emit_loss(torch::tensor(1.0)).has_value());

    const auto records = _metrics();
    ASSERT_EQ(records.size(), 1U);
    EXPECT_EQ(tags_of(records[0].tags), (Tags{{"model", "classifier"}, {"epoch", "3"}}));
}

// The training loop in docs/bridges.md, on a fixed batch: two epochs of
// three steps.
TEST_F(TorchBridgeTest, TrainingLoopEmitsLossAndGradientNormPerStepAndRatesPerEpoch) {
    torch::manual_seed(0);
    torch::nn::Linear model(3, 1);
    torch::optim::SGD optimizer(model->parameters(), torch::optim::SGDOptions(0.05));
    const torch::Tensor inputs = torch::randn({8, 3});
    const torch::Tensor targets = torch::randn({8, 1});
    const int32_t n_epochs = 2;
    const int32_t steps_per_epoch = 3;

    const lumen::bridges::TorchTrainingMetrics metrics("regressor", _core);
    LUMEN_LOOP("epoch", epoch, n_epochs) {
        for (int32_t step = 0; step < steps_per_epoch; ++step) {
            optimizer.zero_grad();
            const torch::Tensor loss = torch::mse_loss(model->forward(inputs), targets);
            loss.backward();
            EXPECT_TRUE(metrics.emit_loss(loss).has_value());
            EXPECT_TRUE(metrics.emit_gradient_norm(*model).has_value());  // before step()
            optimizer.step();
        }
        EXPECT_TRUE(metrics.emit_learning_rates(optimizer).has_value());
    }

    const auto records = _metrics();
    ASSERT_EQ(records.size(), 14U);  // per epoch: 3 x (loss, grad_norm), then 1 rate
    for (std::size_t i = 0; i < records.size(); ++i) {
        const std::size_t in_epoch = i % 7;
        const std::string_view expected_name = in_epoch == 6       ? "torch.learning_rate"
                                               : in_epoch % 2 == 0 ? "torch.loss"
                                                                   : "torch.grad_norm";
        EXPECT_EQ(records[i].name, expected_name) << i;
        EXPECT_EQ(test::tag_of(records[i].tags, "model"), "regressor") << i;
        EXPECT_EQ(test::tag_of(records[i].tags, "epoch"), std::to_string(i / 7)) << i;
    }
    // Gradient descent with a small step lowers the loss on a fixed batch.
    EXPECT_LT(records[7 + 4].value, records[0].value);
}

TEST(TorchBridgeError, CodesCompareWithTheEnumAndHaveMessages) {
    const std::error_code code = bridges::make_error_code(TorchMetricsError::NoGradients);
    EXPECT_EQ(code, TorchMetricsError::NoGradients);
    EXPECT_NE(code, TorchMetricsError::NotAScalar);
    EXPECT_STREQ(code.category().name(), "lumen.torch");
    EXPECT_EQ(code.message(), "no parameter has a gradient");
    EXPECT_EQ(bridges::make_error_code(TorchMetricsError::NotAScalar).message(),
              "tensor is not a single value");
}

class TorchBridgeGlobalCoreTest : public test::GlobalCoreTest {};

TEST_F(TorchBridgeGlobalCoreTest, WithoutACoreArgumentTheRecordsGoToTheGlobalCore) {
    const TorchTrainingMetrics metrics("global");
    EXPECT_TRUE(metrics.emit_loss(torch::tensor(2.0)).has_value());
    core().flush();

    const auto records = _captured->metrics();
    ASSERT_EQ(records.size(), 1U);
    EXPECT_EQ(records[0].name, "torch.loss");
    EXPECT_EQ(records[0].value, 2.0);
}

}  // namespace
}  // namespace lumen
