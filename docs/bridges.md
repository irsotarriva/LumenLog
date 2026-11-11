# Bridges

A bridge is an optional, header-only set of helpers for one framework: it
reads values from the framework's own objects and emits them as Lumen metric
records. There are four, in `include/lumen/bridges/`:

| Header | Framework | What you call |
|---|---|---|
| `lumen/bridges/eigen.hpp` | Eigen 3.3+ | `emit_matrix_summary`, `emit_solver_summary` |
| `lumen/bridges/openmp.hpp` | OpenMP 3.0+ | `emit_openmp_configuration`, `OpenMpWorkBalance` |
| `lumen/bridges/root.hpp` | ROOT 6 | `emit_tree_summary` |
| `lumen/bridges/torch.hpp` | libtorch (PyTorch C++ API) | `TorchTrainingMetrics` |

**No bridge hooks anything.** Nothing is emitted when you call `forward()`,
`backward()`, `TTree::Fill()`, an Eigen product or a parallel loop. A bridge
function emits records when your code calls it, and only then; you decide
where in your loop that is. The reasons are in each framework: the libtorch
C++ frontend has no module forward/backward hooks (those are Python-only),
`TTree::Fill` has no callback, Eigen's expression templates have no hook
point, and OpenMP's tool interface (OMPT) is a runtime plug-in far heavier
than a header.

## Common rules

- **Header-only, and empty without the framework.** Each header is guarded by
  `__has_include` of the framework's main header (`<Eigen/Core>`,
  `<TTree.h>`, `<torch/torch.h>`), or for OpenMP by `_OPENMP >= 200805`
  (OpenMP 3.0, set by `-fopenmp`). Without it the header compiles to nothing,
  so it can be included unconditionally. Link the framework yourself as usual
  (`Eigen3::Eigen`, `OpenMP::OpenMP_CXX`, `${TORCH_LIBRARIES}`,
  `ROOT::Tree`); Lumen itself does not depend on any of them.
- **Namespace** `lumen::bridges`.
- **Which Core.** Every emitting function takes a `lumen::Core&` as its last
  argument, defaulting to the global `lumen::core()`. The tests pass a local
  `Core`; programs normally leave it out.
- **Tags.** Each record carries the bridge's own tags (listed below) first,
  then the ambient context like any other record (`LUMEN_SCOPE`, `LUMEN_LOOP`,
  thread and process tags; see `lumen/context.hpp`). Tag values are text:
  integers are written exactly (`rows=3`).
- **Cost.** A bridge does no work until it is called. What a call costs is
  stated with each function.

## Eigen — `lumen/bridges/eigen.hpp`

Two functions, to call after a step whose result you want to watch.

`emit_matrix_summary(name, matrix)` takes any dense matrix, vector or matrix
expression with floating-point or complex coefficients (the concept
`EigenFloatingMatrix`; integer matrices and `Array`s do not compile). An
expression is evaluated once into a temporary; a plain matrix is read in
place. Cost: one pass for the norm, one for the finiteness check.

| Metric | Value | Tags |
|---|---|---|
| `eigen.matrix.norm` | Frobenius norm (a vector's Euclidean norm), computed with scaling (`blueNorm()`), so finite coefficients as large as 1e300 or as small as 1e-300 give a finite, nonzero norm; NaN if a coefficient is NaN, otherwise infinite if one is infinite or the norm exceeds the largest finite value | `matrix=<name>`, `rows`, `cols` |
| `eigen.matrix.non_finite` | number of NaN or infinite coefficients | `matrix=<name>`, `rows`, `cols` |

`emit_solver_summary(name, solver)` takes an iterative solver
(`ConjugateGradient`, `BiCGSTAB`, `LeastSquaresConjugateGradient`, anything
with `iterations()`, `error()` and `info()`: the concept
`EigenIterativeSolver`). Call it after `solve()`: before `compute()` Eigen
asserts in these accessors, and between `compute()` and `solve()` their values
are not defined.

| Metric | Value | Tags |
|---|---|---|
| `eigen.solver.iterations` | `solver.iterations()` | `solver=<name>`, `info=success\|numerical_issue\|no_convergence\|invalid_input` |
| `eigen.solver.error` | `solver.error()`, Eigen's estimate of the relative residual \|Ax − b\| / \|b\| | as above |

<!-- check: requires=eigen prelude=bridge_eigen wrap=body -->
```cpp
#include <Eigen/IterativeLinearSolvers>
#include "lumen/bridges/eigen.hpp"

Eigen::MatrixXd m(2, 2);
m << 3.0, 4.0, 0.0, 0.0;
lumen::bridges::emit_matrix_summary("weights", m);
// eigen.matrix.norm = 5        matrix=weights rows=2 cols=2
// eigen.matrix.non_finite = 0  matrix=weights rows=2 cols=2

Eigen::ConjugateGradient<Eigen::MatrixXd, Eigen::Lower | Eigen::Upper> solver;
solver.compute(a);
const Eigen::VectorXd x = solver.solve(b);
lumen::bridges::emit_solver_summary("cg", solver);
// eigen.solver.iterations, eigen.solver.error   solver=cg info=success
```

(Eigen's iterative solvers need dynamic-size matrices: with a fixed-size
`Matrix3d`, Eigen's own size assertion fails in `solve()`.)

## OpenMP — `lumen/bridges/openmp.hpp`

`emit_openmp_configuration()` records what the runtime will use. Call it once,
from serial code (inside a parallel region `omp_get_max_threads()` describes a
nested region instead). No tags.

| Metric | Value |
|---|---|
| `openmp.max_threads` | `omp_get_max_threads()`: team size of the next region without a `num_threads` clause |
| `openmp.num_procs` | `omp_get_num_procs()` |

`OpenMpWorkBalance` measures how evenly one parallel region spread its work.
The unit of work is yours (events, iterations, bytes). Construct it before the
region, call `add(units)` inside it from any thread, and `emit()` after the
region has ended. `add()` takes no lock: each thread writes its own
cache-line-sized slot (three OpenMP runtime queries and an addition per call).

<!-- check: requires=openmp prelude=bridge_openmp wrap=body -->
```cpp
#include "lumen/bridges/openmp.hpp"

lumen::bridges::OpenMpWorkBalance balance("events");
#pragma omp parallel for schedule(dynamic)
for (int64_t i = 0; i < n_events; ++i) {
    process(i);
    balance.add(1);
}
balance.emit();
```

| Metric | Value | When |
|---|---|---|
| `openmp.work.total` | units added to thread slots | always |
| `openmp.work.imbalance` | busiest thread's units / mean units per thread of the team: 1 is perfect balance, the team size means one thread did everything | when the total is not 0 and nothing is unattributed |
| `openmp.work.unattributed` | units that could not be given to a thread slot | when not 0 |

All three are tagged `region=<name>` and `threads=<team size>`. The team size
is the largest `omp_get_num_threads()` seen by an `add()` that reached a slot,
and 0 if none did; a thread of the team that never called `add()` counts as
idle. The object has one slot per thread number below `max_team_size`
(constructor argument, default `omp_get_max_threads()` at construction).
`add()` goes to `unattributed` from a thread numbered higher (a `num_threads`
clause asking for more) and from a region nested inside an enclosing region
whose team has more than one thread (thread numbers then repeat across the
inner teams). A region nested only in regions with a team of one thread
(serialised by `if(false)`, `num_threads(1)` or the active-levels limit) is
attributed like an outermost one. Outside any parallel region `add()` counts
as thread 0 of a team of 1.

## ROOT — `lumen/bridges/root.hpp`

`emit_tree_summary(tree)` records the size of a `TTree`. Call it after
`tree.Write()`, or every so many entries while filling. Cost: three accessor
calls.

| Metric | Value | Unit | Tags |
|---|---|---|---|
| `root.tree.entries` | `tree.GetEntries()` | entries | `tree=<tree.GetName()>` |
| `root.tree.uncompressed_bytes` | `tree.GetTotBytes()` | bytes | as above |
| `root.tree.compressed_bytes` | `tree.GetZipBytes()` | bytes | as above |

ROOT adds a basket to both byte counts when it writes the basket out, so while
a tree is being filled they lag behind the entry count; after `tree.Write()`
on a tree attached to a file they cover every entry.

<!-- check: requires=root prelude=bridge_root wrap=body -->
```cpp
#include <TFile.h>
#include <TTree.h>
#include "lumen/bridges/root.hpp"

TFile file("run.root", "RECREATE");
TTree tree("events", "events");      // destroyed before `file`
double energy = 0.0;
tree.Branch("energy", &energy, "energy/D");
for (const double e : {1.0, 2.0, 3.0}) {
    energy = e;
    tree.Fill();
}
tree.Write();
lumen::bridges::emit_tree_summary(tree);
// root.tree.entries = 3, root.tree.uncompressed_bytes, root.tree.compressed_bytes   tree=events
```

An event loop needs no bridge: `Long64_t` is an integer type, so `LUMEN_LOOP`
reports progress over the entries and tags every record with `entry=<i>`:

<!-- check: requires=root prelude=bridge_root wrap=body -->
```cpp
LUMEN_LOOP("entry", i, tree.GetEntries()) {
    tree.GetEntry(i);
    // ... records logged here carry entry=<i>
}
```

## libtorch — `lumen/bridges/torch.hpp`

`TorchTrainingMetrics(model_name)` emits training metrics from a libtorch
loop. Every record is tagged `model=<model_name>`. There is no step or epoch
argument: run the loop under `LUMEN_LOOP` (or `LUMEN_SCOPE`) and the records
carry its tag.

| Method | Metric | Value | Call it |
|---|---|---|---|
| `emit_loss(loss)` | `torch.loss` | `loss.item<double>()` | after computing the loss |
| `emit_gradient_norm(module)` | `torch.grad_norm` | global L2 norm of all parameter gradients, sqrt(Σ‖∇p‖²), the quantity `torch::nn::utils::clip_grad_norm_` compares with its `max_norm`; parameters without a gradient are left out; also returned | after `backward()`, before `step()`/`zero_grad()` |
| `emit_learning_rates(optimizer)` | `torch.learning_rate`, one per parameter group, tagged `group=<index>` | `options().get_lr()` of the group (a scheduler's current value) | whenever, e.g. once per epoch |

Cost: `emit_loss` reads one value from the device. `emit_gradient_norm`
computes one norm per parameter tensor and reads each back (a device
synchronisation per tensor on a GPU).

Errors. Each method returns `std::expected`; on failure nothing is emitted:

| Error (`TorchMetricsError`) | Cause |
|---|---|
| `NotAScalar` | `emit_loss`: the tensor is undefined or has more than one element |
| `NoGradients` | `emit_gradient_norm`: no parameter has a gradient (no `backward()` yet, or all frozen) |
| `FrameworkException` | libtorch threw (for instance `item()` on a meta tensor, or optimizer options without `get_lr()`); the exception's message is also logged as a `Warn` record tagged `model=<name>` |

<!-- check: requires=torch prelude=bridge_torch wrap=body -->
```cpp
#include <torch/torch.h>
#include "lumen/bridges/torch.hpp"

torch::nn::Linear model(3, 1);
torch::optim::SGD optimizer(model->parameters(), torch::optim::SGDOptions(0.05));
const lumen::bridges::TorchTrainingMetrics metrics("regressor");

LUMEN_LOOP("epoch", epoch, n_epochs) {
    for (int32_t step = 0; step < steps_per_epoch; ++step) {
        optimizer.zero_grad();
        const torch::Tensor loss = torch::mse_loss(model->forward(inputs), targets);
        loss.backward();
        if (const auto r = metrics.emit_loss(loss); !r) {
            LUMEN_LOG_WARN("loss not recorded: {}", r.error().message());
        }
        if (const auto norm = metrics.emit_gradient_norm(*model); !norm) {
            LUMEN_LOG_WARN("gradient norm not recorded: {}", norm.error().message());
        }
        optimizer.step();
    }
    if (const auto r = metrics.emit_learning_rates(optimizer); !r) {
        LUMEN_LOG_WARN("learning rate not recorded: {}", r.error().message());
    }
}
// per step: torch.loss, torch.grad_norm   model=regressor epoch=<e>
// per epoch: torch.learning_rate          model=regressor group=0 epoch=<e>
```

## Writing a bridge for another framework

A bridge needs nothing but the public producer API:

- `lumen::metric(name, value).tag(key, value)` for the global core, or
  `lumen::MetricBuilder(core, name, value).tag(...)` for a given `Core`
  (`lumen/record_builder.hpp`). The record is submitted when the builder is
  destroyed, at the end of the statement.
- `lumen::ProgressHandle(core, label, total)` or `lumen::progress(label, total)`
  (`lumen/progress.hpp`) for a progress bar.
- `LUMEN_LOG_*` (`lumen/macros.hpp`) for messages.

Rules the existing bridges follow, and a new one should too:

1. Guard the content with `__has_include(<framework header>)` (or the
   framework's own feature macro) so that the header is empty without it.
2. Do no work until the user calls a function. If the framework offers a
   stable, public callback, a bridge may register one, but only from an object
   the user creates, never from a static initialiser, and its docs must say
   which framework calls then emit records.
3. Take `lumen::Core& core = lumen::core()` last, so that it can be tested
   with a local `Core`.
4. Catch the framework's exceptions at the boundary and return
   `std::expected<T, std::error_code>`.
5. Document every metric name, value, unit and tag next to the function.
6. Add a test under `tests/bridges/` built only when `find_package` finds the
   framework (see `tests/bridges/frameworks.cmake`).

```cpp
#ifndef MY_PROJECT_LUMEN_MYSOLVER_HPP
#define MY_PROJECT_LUMEN_MYSOLVER_HPP

#if __has_include(<mysolver/solver.h>)

#include <mysolver/solver.h>

#include <string_view>

#include "lumen/core.hpp"
#include "lumen/record_builder.hpp"

namespace my_project {

// Emits mysolver.residual = solver.residual(), tagged solver=<name>.
inline void emit_residual(std::string_view name, const mysolver::Solver& solver,
                          lumen::Core& core = lumen::core()) {
    lumen::MetricBuilder(core, "mysolver.residual", solver.residual()).tag("solver", name);
}

}  // namespace my_project

#endif
#endif
```

## Verification

Every bridge has GTest tests under `tests/bridges/` that run it against the
real framework and check the exact metric names, values and tags that reach a
sink, plus a compile check that includes the header alone with the framework
(`CompileCheck.Bridge<Name>WithFramework`) and one that includes every bridge
without any framework (`CompileCheck.BridgesWithoutFrameworks`). The tests,
the compile check and the documentation blocks of a bridge are built only
when CMake finds its framework; otherwise configuring prints
`Lumen: <Framework> bridge tests: NOT BUILT, ...` and they are absent from
ctest, so a passing run never contains a bridge test that did not run.
Setting `LUMEN_REQUIRED_BRIDGE_FRAMEWORKS` (e.g. `Eigen3;OpenMP`) turns a
missing framework into a configure error; every CI job below that tests a
bridge sets it. All of these jobs fail the workflow when a test fails.

| Bridge | Exercised by |
|---|---|
| Eigen | Eigen 3.4.0 (Ubuntu 24.04 `libeigen3-dev`), GCC 13: CI jobs `linux-gcc-debug-asan` (ASan + UBSan) and `bridges (torch)`, and locally |
| OpenMP | GCC 13 libgomp (OpenMP 4.5): the Linux GCC jobs of the main CI matrix except `linux-gcc-tsan` (`linux-gcc-debug-asan` requires it), `bridges (torch)`, and locally. Not built under ThreadSanitizer (libgomp is not instrumented, so TSan reports false races) |
| libtorch | libtorch 2.5.1 CPU (cxx11 ABI), GCC 13: CI job `bridges (torch)` only |
| ROOT | ROOT 6.32 from conda-forge, conda-forge GCC 13: CI job `bridges (root)` only |

The code blocks on this page are compiled by the documentation check against
the same frameworks, and not at all where the framework is missing.

To run the bridge tests yourself, point CMake at the frameworks:

```sh
cmake --preset debug -DLUMEN_ENABLE_DASHBOARD=OFF \
      -DCMAKE_PREFIX_PATH="/path/to/libtorch;$(root-config --prefix)"
cmake --build build/debug
ctest --preset debug -R Bridge
```
