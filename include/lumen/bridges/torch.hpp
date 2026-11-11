#ifndef LUMEN_BRIDGES_TORCH_HPP
#define LUMEN_BRIDGES_TORCH_HPP

// libtorch (PyTorch C++ API) helpers. Nothing here hooks forward(),
// backward() or the optimizer: the libtorch C++ frontend has no module
// hooks, so the training loop calls these where it has the values.
//
//   lumen::bridges::TorchTrainingMetrics metrics("classifier");
//   LUMEN_LOOP("epoch", epoch, n_epochs) {
//       for (auto& batch : *loader) {
//           optimizer.zero_grad();
//           torch::Tensor loss = torch::nll_loss(model->forward(batch.data), batch.target);
//           loss.backward();
//           (void)metrics.emit_loss(loss);
//           (void)metrics.emit_gradient_norm(*model);   // after backward(), before step()
//           optimizer.step();
//       }
//       (void)metrics.emit_learning_rates(optimizer);
//   }
//
// Every record is tagged model=<name>. Records carry the ambient context as
// usual, so the epoch=<n> tag above comes from LUMEN_LOOP; there is no step
// or epoch argument here.
//
// Errors. Each function returns an error instead of emitting when its input
// cannot give a value (TorchMetricsError). libtorch reports failures by
// throwing; such an exception is caught, its message is logged as a Warn
// record (tagged model=<name>) on the same Core, and the function returns
// TorchMetricsError::FrameworkException. Nothing is emitted by a call that
// fails.
//
// The header is empty unless <torch/torch.h> can be included.

#if __has_include(<torch/torch.h>)

#include <torch/torch.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <source_location>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

#include "lumen/core.hpp"
#include "lumen/level.hpp"
#include "lumen/record_builder.hpp"

namespace lumen::bridges {

enum class TorchMetricsError : uint8_t {
    None = 0,
    // emit_loss: the tensor is undefined or does not hold exactly one element.
    NotAScalar,
    // emit_gradient_norm: no parameter of the module has a gradient (backward()
    // has not run, or every parameter has requires_grad == false).
    NoGradients,
    // libtorch threw; the message was logged as a Warn record.
    FrameworkException,
};

class TorchMetricsErrorCategory : public std::error_category {
public:
    [[nodiscard]] const char* name() const noexcept override { return "lumen.torch"; }
    [[nodiscard]] std::string message(int ev) const override {
        switch (static_cast<TorchMetricsError>(ev)) {
            case TorchMetricsError::None:               return "no error";
            case TorchMetricsError::NotAScalar:         return "tensor is not a single value";
            case TorchMetricsError::NoGradients:        return "no parameter has a gradient";
            case TorchMetricsError::FrameworkException: return "libtorch threw an exception";
        }
        return "unknown lumen.torch error";
    }
};

[[nodiscard]] inline const TorchMetricsErrorCategory& torch_metrics_error_category() {
    static const TorchMetricsErrorCategory instance;
    return instance;
}

[[nodiscard]] inline std::error_code make_error_code(TorchMetricsError e) {
    return {static_cast<int>(e), torch_metrics_error_category()};
}

}  // namespace lumen::bridges

template <>
struct std::is_error_code_enum<lumen::bridges::TorchMetricsError> : std::true_type {};

namespace lumen::bridges {

class TorchTrainingMetrics {
public:
    explicit TorchTrainingMetrics(std::string_view model, Core& core = lumen::core())
        : model_(model), core_(&core) {}

    // Emits torch.loss = loss.item<double>().
    [[nodiscard]] std::expected<void, std::error_code> emit_loss(const torch::Tensor& loss) const {
        if (!loss.defined() || loss.numel() != 1) {
            return std::unexpected(make_error_code(TorchMetricsError::NotAScalar));
        }
        double value = 0.0;
        try {
            value = loss.item<double>();
        } catch (const std::exception& e) {
            return framework_error_(e.what());
        } catch (...) {
            return framework_error_("unknown exception");
        }
        MetricBuilder(*core_, "torch.loss", value).tag("model", model_);
        return {};
    }

    // Emits torch.grad_norm: the global L2 norm of the gradients,
    // sqrt(sum over parameters p of ||grad p||^2), the quantity that
    // torch::nn::utils::clip_grad_norm_ compares with its max_norm. Parameters
    // without a gradient are left out. Call it after backward() and before
    // the optimizer step (or zero_grad()). Returns the norm.
    //
    // Cost: one norm per parameter tensor, each read back to the host (a
    // device synchronisation per tensor for a model on a GPU).
    [[nodiscard]] std::expected<double, std::error_code> emit_gradient_norm(
        const torch::nn::Module& module) const {
        double norm = 0.0;
        std::size_t with_gradient = 0;
        try {
            const torch::NoGradGuard no_grad;
            for (const torch::Tensor& parameter : module.parameters()) {
                const torch::Tensor& gradient = parameter.grad();
                if (!gradient.defined()) {
                    continue;
                }
                // hypot accumulates sqrt(a^2 + b^2) without overflowing.
                norm = std::hypot(norm, gradient.norm().item<double>());
                ++with_gradient;
            }
        } catch (const std::exception& e) {
            return framework_error_(e.what());
        } catch (...) {
            return framework_error_("unknown exception");
        }
        if (with_gradient == 0) {
            return std::unexpected(make_error_code(TorchMetricsError::NoGradients));
        }
        MetricBuilder(*core_, "torch.grad_norm", norm).tag("model", model_);
        return norm;
    }

    // Emits torch.learning_rate for each parameter group of the optimizer,
    // tagged group=<index>, read through OptimizerOptions::get_lr() (so a
    // learning-rate scheduler's current value). An optimizer whose options
    // do not implement get_lr() makes libtorch throw: FrameworkException, and
    // no group is emitted.
    [[nodiscard]] std::expected<void, std::error_code> emit_learning_rates(
        const torch::optim::Optimizer& optimizer) const {
        std::vector<double> rates;
        try {
            for (const torch::optim::OptimizerParamGroup& group : optimizer.param_groups()) {
                rates.push_back(group.options().get_lr());
            }
        } catch (const std::exception& e) {
            return framework_error_(e.what());
        } catch (...) {
            return framework_error_("unknown exception");
        }
        for (std::size_t group = 0; group < rates.size(); ++group) {
            MetricBuilder(*core_, "torch.learning_rate", rates[group])
                .tag("model", model_)
                .tag("group", group);
        }
        return {};
    }

    [[nodiscard]] std::string_view model() const noexcept { return model_; }

private:
    [[nodiscard]] std::unexpected<std::error_code> framework_error_(std::string_view what) const {
        RecordBuilder(*core_, LogLevel::Warn, std::source_location::current(), "libtorch: {}", what)
            .tag("model", model_);
        return std::unexpected(make_error_code(TorchMetricsError::FrameworkException));
    }

    std::string model_;
    Core* core_;  // non-owning
};

}  // namespace lumen::bridges

#endif  // __has_include(<torch/torch.h>)

#endif
