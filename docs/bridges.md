# Bridges

Bridges are opt-in headers that hook into third-party frameworks and emit records automatically — zero additional instrumentation required. Include the bridge header after the framework header, and framework operations begin producing Lumen records.

Bridges depend only on the producer API (macros and `lumen::metric()`). They have no dependency on sink internals.

## Available bridges

### Torch (`lumen/bridges/torch.h`)

Hooks into PyTorch training loops. Automatically emits metrics for forward/backward passes.

```cpp
#include "lumen/lumen.h"
#include "lumen/bridges/torch.h"

// Training code unchanged — metrics appear automatically
auto model = MyModel();
model->train();
// MetricRecord: "loss", "grad_norm", "lr" emitted each batch
```

Metrics emitted:
- `train_loss` — scalar loss value after forward pass
- `grad_norm` — gradient norm after backward pass
- `learning_rate` — current learning rate from optimizer

### ROOT (`lumen/bridges/root.h`)

Hooks into ROOT TTree fills and histogram operations.

```cpp
#include "lumen/lumen.h"
#include "lumen/bridges/root.h"

// TTree fills emit progress and metrics automatically
TTree* tree = /* ... */;
tree->Fill();  // ProgressRecord: TTree fill count
```

### Eigen (`lumen/bridges/eigen.h`)

Emits timing for large matrix operations above a configurable size threshold.

```cpp
#include "lumen/lumen.h"
#include "lumen/bridges/eigen.h"

Eigen::MatrixXd A = /* ... */;
Eigen::MatrixXd B = /* ... */;
auto C = A * B;  // MetricRecord: operation duration if matrices exceed threshold
```

### OpenMP (`lumen/bridges/openmp.h`)

Emits thread pool utilization metrics.

```cpp
#include "lumen/lumen.h"
#include "lumen/bridges/openmp.h"

#pragma omp parallel for
for (int i = 0; i < N; i++) {
    // MetricRecord: thread count, work distribution
}
```

## How bridges work

Bridges use framework-specific extension points (hooks, callbacks, template specialization, RAII wrappers) to intercept key operations. They emit records using the standard Lumen producer API — the same `LOG_*` macros and `lumen::metric()` calls available to user code.

A bridge header is guarded with `#ifdef` checks for the framework it wraps. Including a bridge header without the framework present is harmless — it produces no code.

## Writing a custom bridge

A bridge is a standalone header. It should:

1. Be guarded by `#ifdef` for the framework it wraps
2. Depend only on the producer API (`lumen/lumen.h`)
3. Use framework extension points to intercept operations
4. Emit records via `LOG_*`, `lumen::metric()`, or `lumen::progress()`
5. Be opt-in — produce no side effects until explicitly included

Example skeleton:

```cpp
// bridges/my_framework.h
#ifndef LUMEN_BRIDGE_MY_FRAMEWORK_H
#define LUMEN_BRIDGE_MY_FRAMEWORK_H

#ifdef MY_FRAMEWORK_VERSION  // guard: only active when framework is present

#include "lumen/lumen.h"
#include <my_framework/core.h>

namespace lumen::bridge {

struct MyFrameworkHook {
    // Hook into framework callbacks
    static void on_operation(const char* name, double duration) {
        lumen::metric(name, duration).tag("source", "my_framework");
    }
};

// Register hooks at static init
static int __registered = []() {
    my_framework::register_callback(MyFrameworkHook::on_operation);
    return 0;
}();

}  // namespace lumen::bridge

#endif  // MY_FRAMEWORK_VERSION
#endif  // LUMEN_BRIDGE_MY_FRAMEWORK_H
```
