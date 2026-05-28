# Lumen — Implementation Milestones

Branch-per-milestone strategy. Each branch merges into `main` when its section is complete and all tests pass.

| Milestone | Branch | Depends on | Parallel |
|---|---|---|---|
| M1 — Transport | `m1/transport` | — | First |
| M2 — Dispatch & Predicates | `m2/dispatch` | M1 merged | After M1 |
| M3 — Macros, Tagged, Sinks | `m3/sinks` | M1 merged | **Parallel with M2** |
| M4 — Dashboard & Polish | `m4/dashboard` | M2 + M3 merged | After M2+M3 |
| M5 — Extensions | `m5/extensions` | M4 merged | After M4 |

---

## M1 — Transport Layer (`m1/transport`)

The foundation: ring buffers for all three record types, a thread-local formatting arena, and the producer-side commit path.

### Files

- [ ] `include/lumen/detail/arena.h` — Fixed-capacity (4 KB) stack arena, spills to `std::string` on overflow with stderr warning. Provides `char* allocate(size_t)`. Thread-local by default.
- [ ] `include/lumen/detail/ring_buffer.h` — `template<typename T, size_t N> RingBuffer`. Vyukov-style MPSC. Slot with atomic sequence counter. CAS on `head`. Consumer reads `tail` relaxed. DROP_OLDEST / DROP_NEWEST / BLOCK overflow policies.
- [ ] `include/lumen/detail/overflow_policy.h` — `OverflowPolicy` enum (`DROP_OLDEST`, `DROP_NEWEST`, `BLOCK`) + trait helpers.
- [ ] `include/lumen/record.h` — **Update** `RecordBuilder` to hold pointer to arena + ring buffer; destructor formats message into arena, builds record, pushes to ring. `MetricBuilder` and `ProgressHandle` similarly.
- [ ] `include/lumen/core.h` — **Update** `Core` to own 3 `RingBuffer` instances (`LogBuffer`, `MetricBuffer`, `ProgressBuffer`). Expose `emit(LogRecord&&)`, `emit(MetricRecord&&)`, `emit(ProgressRecord&&)` that push to respective buffers.
- [ ] `src/core.cpp` — **Update** wire ring buffers to `emit()`.
- [ ] `tests/test_transport.cpp` — Push records through each ring buffer type. Verify ordering. Verify CAS handles concurrent producers. Verify overflow policies. Verify arena wraps correctly and spill path warns.

### Test expectations

```
test_transport:
  - SingleProducerSingleConsumer  — 1 thread pushes, 1 thread drains, ordering
  - MultiProducerSingleConsumer   — N threads push, 1 drains, no lost records
  - OverflowDropOldest            — buffer fills, oldest dropped, newest delivered
  - OverflowDropNewest            — buffer fills, newest dropped, oldest preserved
  - OverflowBlock                 — producer blocks until consumer drains
  - ArenaBasicAlloc               — allocate within capacity, correct alignment
  - ArenaOverflowSpill            — request > capacity, spills to string, warning logged
```

---

## M2 — Dispatch & Predicates (`m2/dispatch`)

The predicate engine, the dispatch thread that drains all three ring buffers, and the 5-layer context merge.

### Files

- [ ] `include/lumen/predicate.h` — **Update** implement `Predicate::Node` subtypes: `TagEquals`, `LevelAtLeast`, `And`, `Or`, `Not`, `Always`, `Never`. Wire `evaluate()` to tree walk.
- [ ] `src/predicate.cpp` — **Update** all DSL functions (`always()`, `never()`, `level_at_least()`, `tag_equals()`, `tag_exists()`, `operator&&`, `operator||`, `operator!`).
- [ ] `include/lumen/detail/context.h` — Context merge algorithm: merges explicit → instance → scope → thread → process tags. First-wins per key. Linear scan over fixed-size arrays.
- [ ] `include/lumen/core.h` — **Update** add dispatch thread (`std::jthread`), sink registry (`std::vector<SinkEntry>`), condition variable wake. Drain order: Progress → Metric → Log.
- [ ] `src/core.cpp` — **Update** dispatch loop: wake on CV, drain all 3 buffers, evaluate each record against each sink's predicate, call `on_*` on matches. `add_sink()` / `remove_sink()` with mutex guard.
- [ ] `include/lumen/core.h` — **Add** `set_process_tag()` / `set_thread_tag()` storage (thread-local `TagSet<8>` for thread tags, static `TagSet<16>` for process tags).
- [ ] `tests/test_predicate.cpp` — Unit test every predicate node, composition with `&&` `||` `!`. Verify short-circuit. Test `tag_exists`.
- [ ] `tests/test_dispatch.cpp` — End-to-end: emit records → dispatch thread evaluates → correct sink receives matching records. Verify context merge priority.

### Test expectations

```
test_predicate:
  - AlwaysTrue
  - NeverFalse
  - LevelAtLeastMatches         — record at or above threshold
  - LevelAtLeastMisses          — record below threshold
  - TagEqualsMatch
  - TagEqualsMiss
  - TagExistsMatch
  - AndShortCircuit
  - OrShortCircuit
  - NotInverts
  - CompositeEveryCombination

test_dispatch:
  - RecordRoutedToCorrectSink   — only sink whose predicate matches gets record
  - RecordRoutedToMultipleSinks — record matches two sinks, both receive it
  - RecordRoutedToNoSink        — no sink matches, record is discarded
  - ContextMergePriority        — explicit tag overrides scope tag, etc.
  - ProcessTagsAttached         — every record carries process tags
  - ThreadTagsAttached          — every record from thread carries thread tags
  - DispatchOrder               — Progress delivered before Metric before Log
```

---

## M3 — Macros, Tagged, Sinks (`m3/sinks`)

The producer-facing API: macros, scope helpers, `lumen::Tagged` mixin, and the three built-in sinks.

### Files

- [ ] `include/lumen/record.h` — **Add** `LUMEN_IF_ENABLED(LEVEL, expr)` macro. `#define LOG_TRACE`, `LOG_DEBUG`, `LOG_INFO`, `LOG_WARN`, `LOG_ERROR`, `LOG_FATAL` — each calls `make_builder()` with `std::source_location::current()`, conditionally on `LUMEN_IF_ENABLED`. `LOG_FATAL` calls `std::terminate` after commit.
- [ ] `include/lumen/detail/scope_stack.h` — Thread-local linked list of `ScopeFrame`. `LUMEN_SCOPE(key, value)`, `LUMEN_LOOP(label, var, total)`, `LUMEN_FRAME_SCOPE(frame_var)` macros. RAII push/pop.
- [ ] `include/lumen/record.h` — **Add** `LUMEN_MEMBER_SCOPE` macro.
- [ ] `include/lumen/tagged.h` — **Update** implement `lumen_tag()` to store in `__tags` member. Implement `lumen_scope()` that returns RAII guard setting a thread-local pointer to the instance's tag set.
- [ ] `src/tagged.cpp` — Tagged implementation.
- [ ] `include/lumen/sink.h` — **Update** `TerminalSink::Config` with level colors, time format, inline tags. Update `FileSink::Config` fields.
- [ ] `src/sink.cpp` — **Update** implement `NullSink` (trivial), `TerminalSink` with ANSI color output per level config, `FileSink` with async write thread + rotation (configurable size, max files), `JsonSink` with NDJSON output.
- [ ] `tests/test_macros.cpp` — Verify macros compile to nothing below threshold. Verify `LOG_INFO(...).tag(...)` chaining works. Verify `LOG_FATAL` terminates. Verify source location is captured.
- [ ] `tests/test_scope.cpp` — Verify `LUMEN_SCOPE` tags present inside scope, absent outside. Verify `LUMEN_LOOP` progress records emitted per iteration. Verify RAII under exception.
- [ ] `tests/test_tagged.cpp` — Verify `lumen::Tagged` instance tags merged into records emitted from member functions with `LUMEN_MEMBER_SCOPE`.
- [ ] `tests/test_sinks.cpp` — Verify `NullSink` discards. Verify `TerminalSink` produces ANSI output. Verify `FileSink` writes to disk. Verify `JsonSink` produces valid NDJSON. Verify sink `flush()` completes pending writes.

### Test expectations

```
test_macros:
  - MacroElidesBelowThreshold   — DEBUG expands to ((void)0) when LUMEN_MIN_LEVEL=INFO
  - MacroCallsRecordBuilder     — LOG_INFO("msg") returns RecordBuilder&
  - MacroChainsTags             — LOG_INFO("msg").tag("k","v") commits with tag
  - SourceLocationCaptured      — file, function, line populated correctly
  - FatalCallsTerminate         — LOG_FATAL calls std::terminate (EXPECT_DEATH)

test_scope:
  - ScopeTagsPresent            — tag visible inside LUMEN_SCOPE block
  - ScopeTagsAbsentOutside      — tag gone after block
  - LoopEmitsProgress           — LUMEN_LOOP emits N progress records
  - LoopTagsAttached            — loop_label, iteration, total on inner records
  - ScopeRaiiOnException        — scope pops on throw (no leak)

test_tagged:
  - InstanceTagsOnMethod        — tags present when LUMEN_MEMBER_SCOPE used
  - InstanceTagsAbsentWithout   — no instance tags without macro

test_sinks:
  - NullSinkDiscardsAll
  - TerminalSinkAnsiOutput
  - FileSinkWritesToDisk
  - FileSinkRotates
  - JsonSinkValidNdjson
  - FlushCompletesPendingWrites
```

---

## M4 — Dashboard & Polish (`m4/dashboard`)

FTXUI-powered terminal dashboard, performance benchmarks, comprehensive error-path tests, LaTeX support.

### Files

- [ ] `src/sink.cpp` — **Update** `TerminalSink` with FTXUI dashboard mode: split view (dashboard top, log scroll region bottom). Dashboard shows sparklines per configured metric, one row per active progress bar. Gated by `#ifdef LUMEN_ENABLE_DASHBOARD`.
- [ ] `include/lumen/sink.h` — **Update** `TerminalSink::Config` with `enable_dashboard`, `dashboard_metrics`.
- [ ] `tests/test_dashboard.cpp` — FTXUI dashboard rendering tests (if dashboard enabled).
- [ ] `tests/test_latex.cpp` — LaTeX tag handling (if latex enabled).
- [ ] `tests/bench_transport.cpp` — Emit throughput benchmark (records/sec, M producers).
- [ ] Error-path tests across all existing test files — ensure every `// TODO:` gap is covered.
- [ ] `include/lumen/detail/error.h` — `std::error_code` enum + `std::error_category` for all Lumen error codes.

### Test expectations

```
test_dashboard:
  - DashboardRendersMetrics     — metric sparklines visible in dashboard mode
  - DashboardRendersProgress    — progress bars visible
  - DashboardUpdatesOnDispatch  — new records update dashboard

bench_transport:
  - reports records/sec for 1/2/4/8 producer threads
```

---

## M5 — Extensions (`m5/extensions`)

Python bindings and framework bridges. Independent headers that depend only on the producer API.

### Files

- [ ] `python/lumen.cpp` — pybind11 module exposing `lumen.info()`, `lumen.warn()`, `lumen.metric()`, `lumen.progress()`, `lumen.core()`, `lumen.set_process_tag()`, `TerminalSink`, `FileSink`, `JsonSink`, `Predicate` builder functions.
- [ ] `python/logging_handler.py` — Python `logging.Handler` subclass that forwards to Lumen.
- [ ] `bridges/torch.h` — PyTorch hook on `nn::Module` forward/backward, emits loss, grad_norm, lr as MetricRecords.
- [ ] `bridges/root.h` — ROOT TTree fill hook.
- [ ] `bridges/eigen.h` — Eigen large-matrix timing.
- [ ] `bridges/openmp.h` — OpenMP thread pool metrics.
- [ ] `tests/test_python.py` — Python-side integration tests.
- [ ] `tests/test_bridge_torch.py` — If torch available, verify bridge emits.

### Test expectations

```
Python:
  - info/warn/error records reach C++ dispatch thread
  - metric() creates MetricRecords in same broker
  - progress() creates ProgressRecords
  - TerminalSink works from Python
  - Python-side logging.Handler forwards to Lumen
```

---

## Branch Workflow

```
   main ── m1/transport ────► merge ── m2/dispatch ────► merge ──► ...
                         ┌──► merge ── m3/sinks ────► merge
                         └ (parallel after M1)
```

1. Start from `main`
2. Create branch: `git checkout -b m1/transport`
3. Implement, test, commit (conventional commits: `feat:`, `test:`, `refactor:`)
4. Verify: `cmake --preset debug && cmake --build build/debug && ctest --preset debug`
5. Merge into `main`
6. Next agent starts from `main`

For parallel work (M2 + M3 after M1 merges): each agent branches from `main`, works independently, merges back in any order. Both must pass all tests.
