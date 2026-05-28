# Sinks

Sinks are the consumer side of Lumen. They receive records that match their predicate and decide what to do with them — format, color, write to disk, forward over the network, render to a dashboard.

## Sink base class

All sinks inherit from `lumen::Sink`:

```cpp
class Sink {
public:
    virtual void on_log(const LogRecord&)     {}
    virtual void on_metric(const MetricRecord&) {}
    virtual void on_progress(const ProgressRecord&) {}
    virtual void flush() = 0;
    virtual ~Sink() = default;
};
```

The dispatch thread calls these methods sequentially for matching records. Implementations **must be non-blocking** — offload I/O to internal threads.

## Built-in sinks

### TerminalSink

Colorized output to stderr with an optional FTXUI dashboard.

```cpp
#include "lumen/sink.h"

lumen::TerminalSink::Config cfg;

// Per-level ANSI colors
cfg.colors[lumen::DEBUG] = lumen::AnsiColor::Cyan;
cfg.colors[lumen::INFO]  = lumen::AnsiColor::White;
cfg.colors[lumen::WARN]  = lumen::AnsiColor::Yellow;
cfg.colors[lumen::ERROR] = lumen::AnsiColor::Red;
cfg.colors[lumen::FATAL] = lumen::AnsiColor{ .fg = 196, .bold = true };

// Timestamp format (strftime-style)
cfg.time_format = "%H:%M:%S";

// Which tags to show inline in the log line
cfg.inline_tags = { "subsystem", "event_id" };

// Dashboard (requires LUMEN_ENABLE_DASHBOARD=ON)
cfg.dashboard_metrics = { "loss", "gpu_mem_mb", "fps" };
cfg.dashboard_height = 5;  // lines reserved at top

auto sink = std::make_unique<lumen::TerminalSink>(cfg);
```

The dashboard renders a split view: progress bars and metric sparklines at the top, scrollable log lines below. No separate API — any metric emitted to a terminal sink with a dashboard enabled appears automatically.

Without `LUMEN_ENABLE_DASHBOARD`, the terminal sink renders log lines only.

### FileSink

Async file output with rotation.

```cpp
lumen::FileSink::Config cfg;
cfg.path         = "run_{date}.log";  // {date} substituted at open time
cfg.rotate_mb    = 256;               // rotate after 256 MB
cfg.max_files    = 5;                 // keep last 5 rotated files
cfg.format       = lumen::FileFormat::PlainText;  // PlainText, KeyValue, or Json

auto sink = std::make_unique<lumen::FileSink>(cfg);
```

The file sink maintains an internal write thread. `on_log` enqueues into a small internal queue and returns immediately.

### JsonSink

Newline-delimited JSON (NDJSON), one object per record:

```json
{"type":"log","level":3,"message":"cluster energy 34.2 GeV","tags":{"subsystem":"calorimeter"},"timestamp":1748486400000000000}
{"type":"metric","name":"loss","value":0.0314,"tags":{"epoch":"5"},"timestamp":1748486400001000000}
{"type":"progress","label":"training","current":500,"total":1000,"tags":{},"timestamp":1748486400002000000}
```

```cpp
auto sink = std::make_unique<lumen::JsonSink>("run.jsonl");
```

Suitable for piping to `jq`, forwarding to Loki, or loading into pandas with `pd.read_json("run.jsonl", lines=True)`.

### NullSink

Discards everything. Useful for benchmarking the broker overhead in isolation.

```cpp
auto sink = std::make_unique<lumen::NullSink>();
```

## Custom sinks

Implement the `Sink` interface:

```cpp
class NetworkSink : public lumen::Sink {
    std::thread __worker;
    lumen::detail::RingBuffer<lumen::LogRecord, 1024> __queue;  // internal queue
    std::atomic<bool> __running{true};

public:
    NetworkSink() {
        __worker = std::thread([this] {
            while (__running) {
                // Drain internal queue, write to socket
                // ...
            }
        });
    }

    void on_log(const lumen::LogRecord& r) override {
        __queue.push(r);   // non-blocking — just enqueue and return
    }

    void flush() override {
        // Wait for internal queue to drain
    }

    ~NetworkSink() override {
        __running = false;
        __worker.join();
    }
};
```

Key rules for custom sinks:
- `on_log` / `on_metric` / `on_progress` are called from the dispatch thread and **must not block**
- Defer I/O to an internal thread
- `flush()` must complete all pending I/O
- Sinks are owned by `Core` via `unique_ptr` — registration transfers ownership

## Registering sinks

```cpp
auto& core = lumen::core();

// Each sink gets a predicate that filters which records it receives
using namespace lumen;

core.add_sink(std::make_unique<TerminalSink>(cfg), level_at_least(INFO));
core.add_sink(std::make_unique<FileSink>("errors.log"), level_at_least(ERROR));
core.add_sink(std::make_unique<JsonSink>("calo.jsonl"),
    tag_equals("subsystem", "calorimeter"));

// Store the SinkId for later removal
lumen::SinkId terminal_id = core.add_sink(/* ... */);
auto removed = core.remove_sink(terminal_id);  // std::expected<unique_ptr<Sink>, error_code>
```

Sinks can be added and removed at any time (infrequent operations, mutex-protected).
