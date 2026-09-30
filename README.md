# Lumen

**Zero-instrumentation structured telemetry for C++20.**

Producers write only semantic content. Consumers receive maximally rich, filterable records — automatically enriched with tags from five layers of context.

```
LOG_INFO("cluster energy {:.2f} GeV", energy)
    .tag("subsystem", "calorimeter");
```

Designed as core infrastructure for any project.

---

## Philosophy

Lumen is built on the idea that **the producer should not carry the cost of the consumer's needs.**

Traditional logging forces every call site to decide what context to attach, what format to use, where to write, and whether the record matters. This couples the producer to every consumer — change your output format, and you edit thousands of log lines. Add a new filter, and every call site needs new annotations.

Lumen inverts this. Producers emit only what they uniquely know — a message, a severity, and optionally explicit tags. Everything else is assembled automatically from ambient context.

### The producer-consumer model

```
Producer                      Lumen                      Consumer
────────                      ─────                      ────────
Writes message + level   →    Merges context         →   Receives full record
Optional .tag() calls         Evaluates predicates       Filters by predicate
No formatting decisions        Dispatches to sinks       Chooses format & destination
```

**Producers** call `LOG_INFO`, `lumen::metric`, or `lumen::progress`. That's the entire producer API. No decision about output format, destination, or filtering.

**Context** is assembled from five layers at emit time. Higher layers override lower layers when the same key appears:

1. **Explicit** — `.tag("k", "v")` on the emit call (always wins)
2. **Instance** — tags set on the enclosing object via `lumen::Tagged`
3. **Scope** — tags pushed by `LUMEN_SCOPE` for the current lexical block
4. **Thread** — tags registered once per thread
5. **Process** — tags registered once at startup

This means a class can tag itself once in its constructor, and every log call from any of its methods automatically carries those tags. A loop wrapper can tag every record inside the loop body. Process-wide identifiers (job ID, hostname, rank) are attached to every record without a single extra line at the call site.

**Consumers** are sinks. Each sink subscribes to a subset of the record stream via composable predicates (`level_at_least(WARN) && tag_equals("subsystem", "calorimeter")`). Sinks decide format, color, destination, and layout — independently of each other and of the producers.

### Zero-instrumentation

In production builds, records below a configurable level threshold compile to nothing. If `LUMEN_MIN_LEVEL=WARN`, every `LOG_DEBUG` and `LOG_INFO` call in the entire codebase is `((void)0)`. No formatting cost, no tag construction, no allocation, no function call. The processor never sees them.

Bridges extend this further — include a header and framework calls (PyTorch forward/backward, ROOT TTree fills, Eigen matrix operations, OpenMP thread pools) automatically emit records with zero changes to user code.

---

## Requirements

- **C++23** compiler (Clang 17+, GCC 14+, MSVC 2022+)
- **CMake** 3.28+
- **vcpkg** (manifest mode, auto-resolved via CMake preset)

Optional dependencies (auto-resolved by vcpkg):
- **FTXUI** — terminal dashboard (`LUMEN_ENABLE_DASHBOARD=ON`, default)
- **pybind11** — Python bindings (`LUMEN_ENABLE_PYTHON=OFF`, default)

---

## Installation

```sh
# Clone
git clone https://github.com/irsotarriva/LumenLog.git
cd lumen

# Configure & build (debug, with tests and examples)
cmake --preset debug
cmake --build build/debug

# Run tests
ctest --preset debug

# Release build (no tests/examples, optimized)
cmake --preset release
cmake --build build/release
```

### CMake integration

```cmake
# In your project's CMakeLists.txt
add_subdirectory(path/to/lumen)
target_link_libraries(your_target PRIVATE lumen::lumen)
```

### Compile-time options

| Flag | Default | Effect |
|---|---|---|
| `LUMEN_MIN_LEVEL` | `DEBUG` | Elides records below this level (TRACE/DEBUG/INFO/WARN/ERROR/FATAL) |
| `LUMEN_ENABLE_DASHBOARD` | `ON` | FTXUI terminal dashboard |
| `LUMEN_ENABLE_LATEX` | `OFF` | LaTeX math rendering in sinks |
| `LUMEN_ENABLE_PYTHON` | `OFF` | pybind11 Python bindings |
| `LUMEN_ENABLE_REFLECTION` | `AUTO` | C++26 class-name harvesting |

---

## Usage

### Quick start

```cpp
#include "lumen/lumen.h"

int main() {
    auto& core = lumen::core();

    // Register a terminal sink that receives everything
    core.add_sink(
        std::make_unique<lumen::TerminalSink>(),
        lumen::always()
    );

    // Set process-wide tags
    core.set_process_tag("app", "analysis");
    core.set_process_tag("run_id", "42");

    // Emit logs — simplest form
    LOG_INFO("starting event processing");

    // With explicit tags
    LOG_WARN("energy out of range, got {:.2f}", e)
        .tag("subsystem", "calorimeter")
        .tag("event_id", "4821");

    // Emit metrics
    lumen::metric("loss", 0.0314);
    lumen::metric("fps", 60.0).tag("scene", "main_menu");

    // Track progress
    auto job = lumen::progress("training", 1000);
    for (int i = 0; i < 1000; i++) {
        train_step();
        job.tick();
    }
    // ProgressHandle destructor auto-finishes

    core.flush();  // drain all records before exit
}
```

### Context layers in practice

```cpp
class JetFinder : public lumen::Tagged {
public:
    JetFinder(int run, std::string algo) {
        lumen_tag("class", "JetFinder");
        lumen_tag("run_id", std::to_string(run));
        lumen_tag("algorithm", std::move(algo));
    }

    void cluster(const EventData& ev) {
        LUMEN_MEMBER_SCOPE;  // activates instance tags for this call
        LOG_DEBUG("starting cluster step");
        // record carries: class=JetFinder, run_id=42, algorithm=antikt
    }
};

void process_run(const std::string& run) {
    LUMEN_SCOPE("run_id", run);       // scoped to this block
    LOG_INFO("run started");           // carries run_id
    {
        LUMEN_SCOPE("event_id", "7");  // nested scopes merge
        LOG_INFO("processing event");  // carries both run_id and event_id
    }
}

void frame_loop(float dt) {
    LUMEN_FRAME_SCOPE(frame_counter, dt) {
        update(dt);
        render();
        // all records carry frame_number and delta_time_ms
    }
}

// Loop with automatic progress + tags
LUMEN_LOOP("training batches", batch, num_batches) {
    forward_pass(batch);
    // records carry: loop_label="training batches", iteration=batch
    // progress bar auto-updates each iteration
}
```

### Filtering with predicates

```cpp
using namespace lumen;

// A sink that only sees warnings and above from the calorimeter
core.add_sink(
    std::make_unique<lumen::TerminalSink>(),
    level_at_least(WARN) && tag_equals("subsystem", "calorimeter")
);

// A JSON sink for all metrics regardless of source
core.add_sink(
    std::make_unique<lumen::JsonSink>("metrics.jsonl"),
    always()
);

// A file sink for a specific run, excluding suppressed records
core.add_sink(
    std::make_unique<lumen::FileSink>("run_42.log"),
    tag_equals("run_id", "42") && !tag_exists("suppress")
);

// The same filter as text, e.g. from a config file or a CLI flag
if (auto query = lumen::parse_predicate("run_id == 42 && altitude < 70000 && !exists(suppress)")) {
    core.add_sink(std::make_unique<lumen::FileSink>("low.log"), std::move(*query));
}
```

### Custom sinks

```cpp
class MySink : public lumen::Sink {
public:
    void on_log(const lumen::LogRecord& r) override {
        // r.message, r.level, r.tags, r.source — all populated
        // Must be non-blocking! Offload I/O to your own thread if needed.
    }
    void on_metric(const lumen::MetricRecord& r) override { /* ... */ }
    void on_progress(const lumen::ProgressRecord& r) override { /* ... */ }
    void flush() override { /* drain internal queue */ }
};
```

### Bridges (zero-instrumentation for frameworks)

```cpp
#include "lumen/lumen.h"
#include "lumen/bridges/torch.h"   // auto-logs forward/backward, loss, lr

// From here, torch::nn::Module subclasses automatically emit metrics.
// No changes to training code required.

auto model = MyModel();
model->train();  // MetricRecord: "loss", "grad_norm", "lr" emitted automatically
```

### Python bindings

```python
import lumen_bindings as lumen

lumen.info("analysis starting")
lumen.metric("loss", 0.0314)

bar = lumen.progress("training", 1000)
bar.tick()

lumen.core().add_sink(lumen.TerminalSink(), lumen.level_at_least("INFO"))
```

---

## Documentation

Full documentation is available in the [`docs/`](docs/index.md) folder:

| Document | Contents |
|---|---|
| [Getting Started](docs/getting-started.md) | Requirements, installation, first program |
| [Philosophy](docs/philosophy.md) | Producer-consumer model, context layers, design rationale |
| [Usage Guide](docs/usage.md) | Logging, metrics, progress, scopes, tags |
| [Sinks](docs/sinks.md) | Built-in sinks, configuration, custom sinks |
| [Predicates](docs/predicates.md) | Predicate DSL reference and examples |
| [API Reference](docs/api.md) | Complete public API |
| [Bridges](docs/bridges.md) | Framework integration (Torch, ROOT, Eigen, OpenMP) |

---

## License

[MIT](LICENSE) — free to use, copy, modify, merge, publish, distribute, sublicense, and sell. See the license file for full terms.
