#ifndef LUMEN_BRIDGES_TORCH_H
#define LUMEN_BRIDGES_TORCH_H

#if __has_include(<torch/torch.h>)

#include <torch/torch.h>
#include <string>
#include <string_view>

#include "lumen/lumen.h"

namespace lumen::bridges {

class TorchHook {
public:
    explicit TorchHook(std::string_view prefix = "")
        : __prefix(prefix) {}

    void on_batch_complete(const torch::nn::Module& module,
                           double loss,
                           double lr = 0.0) {
        auto m = lumen::metric("torch.loss", loss);
        if (!__prefix.empty()) m.tag("prefix", __prefix);

        auto total_norm = 0.0;
        for (const auto& p : module.parameters()) {
            if (p.grad().defined()) {
                total_norm += p.grad().norm().item<double>();
            }
        }
        lumen::metric("torch.grad_norm", total_norm)
            .tag("prefix", __prefix.empty() ? "model" : __prefix);

        if (lr > 0.0) {
            lumen::metric("torch.learning_rate", lr)
                .tag("prefix", __prefix.empty() ? "model" : __prefix);
        }
    }

    void on_epoch_start(uint64_t epoch) {
        lumen::metric("torch.epoch", static_cast<double>(epoch))
            .tag("prefix", __prefix.empty() ? "model" : __prefix)
            .tag("phase", "start");
    }

    void on_epoch_end(uint64_t epoch) {
        lumen::metric("torch.epoch", static_cast<double>(epoch))
            .tag("prefix", __prefix.empty() ? "model" : __prefix)
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
    std::string __prefix;
};

inline TorchHook make_torch_hook(std::string_view prefix = "") {
    return TorchHook(prefix);
}

}  // namespace lumen::bridges

#endif

#endif
