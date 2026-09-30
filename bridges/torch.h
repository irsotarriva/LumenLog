#ifndef LUMEN_BRIDGES_TORCH_H
#define LUMEN_BRIDGES_TORCH_H

#if has_include_(<torch/torch.h>)

#include <torch/torch.h>
#include <string>
#include <string_view>

#include "lumen/lumen.h"

namespace lumen::bridges {

class TorchHook {
public:
    explicit TorchHook(std::string_view prefix = "")
        : prefix_(prefix) {}

    void on_batch_complete(const torch::nn::Module& module,
                           double loss,
                           double lr = 0.0) {
        auto m = lumen::metric("torch.loss", loss);
        if (!prefix_.empty()) m.tag("prefix", prefix_);

        auto total_norm = 0.0;
        for (const auto& p : module.parameters()) {
            if (p.grad().defined()) {
                total_norm += p.grad().norm().item<double>();
            }
        }
        lumen::metric("torch.grad_norm", total_norm)
            .tag("prefix", prefix_.empty() ? "model" : prefix_);

        if (lr > 0.0) {
            lumen::metric("torch.learning_rate", lr)
                .tag("prefix", prefix_.empty() ? "model" : prefix_);
        }
    }

    void on_epoch_start(uint64_t epoch) {
        lumen::metric("torch.epoch", static_cast<double>(epoch))
            .tag("prefix", prefix_.empty() ? "model" : prefix_)
            .tag("phase", "start");
    }

    void on_epoch_end(uint64_t epoch) {
        lumen::metric("torch.epoch", static_cast<double>(epoch))
            .tag("prefix", prefix_.empty() ? "model" : prefix_)
            .tag("phase", "end");
    }

    template <typename Optimizer>
    void on_optimizer_step(const Optimizer& /* optimizer */,
                           double loss,
                           double lr) {
        lumen::metric("torch.loss", loss);
        if (lr > 0.0) {
            lumen::metric("torch.learning_rate", lr);
        }
    }

private:
    std::string prefix_;
};

inline TorchHook make_torch_hook(std::string_view prefix = "") {
    return TorchHook(prefix);
}

}  // namespace lumen::bridges

#endif

#endif
