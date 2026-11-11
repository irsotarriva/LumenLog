// Before the libtorch fragments of README.md and docs/bridges.md: the data
// and the model the training loops use. Compiled, never linked.
#ifndef LUMEN_TESTS_DOCS_PRELUDES_BRIDGE_TORCH_HPP
#define LUMEN_TESTS_DOCS_PRELUDES_BRIDGE_TORCH_HPP

#include <torch/torch.h>

#include <cstdint>

#include "lumen/lumen.hpp"

extern const torch::Tensor inputs;
extern const torch::Tensor targets;
extern torch::nn::Linear model;
extern torch::optim::SGD optimizer;
inline constexpr int32_t n_epochs = 3;
inline constexpr int32_t steps_per_epoch = 4;

#endif
