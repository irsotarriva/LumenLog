#include "interpreter_exit.hpp"

#include <pybind11/pybind11.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <format>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "errors.hpp"
#include "lumen/detail/immortal.hpp"
#include "lumen/error.hpp"
#include "registration.hpp"

namespace py = pybind11;

namespace lumen::python {
namespace {

using Clock = std::chrono::steady_clock;

// How often the exit hook's waits look at their condition, without the GIL,
// and how often they take the GIL to look for signals. Taking it more often
// would slow down the delivery to Python sinks that the hook waits for.
inline constexpr std::chrono::milliseconds exit_poll_interval{1};
inline constexpr std::chrono::milliseconds exit_signal_interval{50};

// The exit priority of the hook in a multiprocessing child (see "fork()" in
// the header): below the finalizers multiprocessing registers itself.
inline constexpr int32_t multiprocessing_exit_priority = -100;

// What the exit hook found, for the report at the end of finalisation.
struct ExitOutcome {
    bool interrupted = false;  // a wait ended because of a signal
    // Records still queued when the Python sinks were removed, if the
    // delivery of step 2 had not finished.
    std::optional<uint64_t> queued_at_detach;
    bool callback_still_running = false;
    bool other_call_still_running = false;  // a call that is not a sink callback
};

// Everything the exit hook coordinates. One instance for the process, never
// destroyed (a C++ thread may still construct a PythonCall during static
// destruction; it must find the gate closed, not a destroyed object).
//
// Lock rule: a thread holding `mutex` never waits for the GIL. Threads that
// take it with the GIL held only do work that cannot wait for the GIL either
// (the GIL is re-entrant for them).
struct ExitState {
    std::mutex mutex;
    bool exit_hook_ran = false;        // guarded by mutex
    std::vector<SinkId> python_sinks;  // guarded by mutex
    bool python_diagnostics = false;   // guarded by mutex
    DiagnosticHandler diagnostics =
        stderr_diagnostics();                     // guarded by mutex: the one installed last
    ExitOutcome outcome;                          // guarded by mutex
    std::atomic<bool> records_open{true};         // see admit_python_record
    std::atomic<bool> sink_callbacks_open{true};  // see PythonCall
    std::atomic<bool> gate_open{true};            // see PythonCall
    std::atomic<uint64_t> calls_in_python{0};
    std::atomic<uint64_t> other_calls_in_python{0};  // those of kind Other
    std::atomic<uint64_t> records_refused{0};
    std::atomic<uint64_t> records_undelivered{0};
    // `mutex`, held from the os.register_at_fork "before" hook to "after";
    // touched only by the thread that forks, with the GIL held.
    std::optional<std::unique_lock<std::mutex>> fork_lock;
};

[[nodiscard]] ExitState& exit_state() {
    static detail::Immortal<ExitState> instance;
    return instance.get();
}

[[nodiscard]] bool interpreter_can_run_code() noexcept {
#if PY_VERSION_HEX >= 0x030D0000
    return Py_IsInitialized() != 0 && Py_IsFinalizing() == 0;
#else
    return Py_IsInitialized() != 0 && _Py_IsFinalizing() == 0;
#endif
}

void raise_after_exit() {
    PyErr_SetString(PyExc_RuntimeError,
                    "the interpreter is exiting; Python sinks and diagnostic handlers can no "
                    "longer be registered");
    throw py::error_already_set();
}

enum class WaitResult : uint8_t {
    Done,
    TimedOut,
    Interrupted
};

// Called with the GIL held, on the main thread (atexit functions run there),
// between two waits without the GIL: whether a signal arrived (Ctrl-C).
[[nodiscard]] bool interrupted_now() {
    if (PyErr_CheckSignals() == 0) {
        return false;
    }
    // The interpreter is already exiting: the interruption is honoured by
    // waiting no longer, so the exception itself is not needed.
    PyErr_Clear();
    return true;
}

// Waits until `done()` or `deadline`; `done` is called without the GIL.
template <std::predicate F>
[[nodiscard]] WaitResult wait_until(F done, Clock::time_point deadline) {
    while (true) {
        {
            const py::gil_scoped_release release;
            const Clock::time_point look_for_signals =
                std::min(Clock::now() + exit_signal_interval, deadline);
            while (!done() && Clock::now() < look_for_signals) {
                std::this_thread::sleep_for(exit_poll_interval);
            }
        }
        if (done()) {
            return WaitResult::Done;
        }
        if (Clock::now() >= deadline) {
            return WaitResult::TimedOut;
        }
        if (interrupted_now()) {
            return WaitResult::Interrupted;
        }
    }
}

// Step 2: Core::flush_for in slices, looking for signals between them. A
// later slice asks for a flush again, which the dispatch thread joins to the
// one in progress. Any outcome but a timeout (done, or a Core that has shut
// down) leaves nothing to wait for.
[[nodiscard]] WaitResult flush_until(Core& c, Clock::time_point deadline) {
    while (true) {
        std::expected<void, std::error_code> flushed;
        {
            const py::gil_scoped_release release;
            const auto left =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
            flushed =
                c.flush_for(std::clamp(left, std::chrono::milliseconds{0}, exit_signal_interval));
        }
        if (flushed || flushed.error() != make_error_code(LumenError::FlushTimedOut)) {
            return WaitResult::Done;
        }
        if (Clock::now() >= deadline) {
            return WaitResult::TimedOut;
        }
        if (interrupted_now()) {
            return WaitResult::Interrupted;
        }
    }
}

// sys.getswitchinterval(), or nothing if it fails. Through the C API, which
// reports a Python error as a set error rather than by throwing; the error is
// shown through sys.unraisablehook, not raised from the exit hook.
[[nodiscard]] std::optional<double> switch_interval() noexcept {
    PyObject* const get = PySys_GetObject("getswitchinterval");  // non-owning: borrowed
    if (get == nullptr) {
        return std::nullopt;
    }
    const auto value = py::reinterpret_steal<py::object>(PyObject_CallNoArgs(get));
    const double seconds = value ? PyFloat_AsDouble(value.ptr()) : -1.0;
    if (PyErr_Occurred() != nullptr) {
        PyErr_WriteUnraisable(get);
        return std::nullopt;
    }
    return seconds;
}

// sys.setswitchinterval(seconds); errors as for switch_interval().
void set_switch_interval(double seconds) noexcept {
    PyObject* const set = PySys_GetObject("setswitchinterval");  // non-owning: borrowed
    if (set == nullptr) {
        return;
    }
    const auto argument = py::reinterpret_steal<py::object>(PyFloat_FromDouble(seconds));
    const auto result =
        argument ? py::reinterpret_steal<py::object>(PyObject_CallOneArg(set, argument.ptr()))
                 : py::object();
    if (!result) {
        PyErr_WriteUnraisable(set);
    }
}

// Every record a Python sink receives needs the GIL, and a thread that waits
// for it gets it only after the switch interval (5 ms by default) when other
// Python threads keep running, as daemon threads still do at exit. A shorter
// interval while the hook delivers lets Python sinks receive many more
// records within exit_delivery_limit. The interpreter's interval is put back
// afterwards. Constructed and destroyed with the GIL held. Changing the
// interval is only a speed-up: when it fails, delivery goes on at the
// interpreter's own pace.
class ShortSwitchInterval {
public:
    ShortSwitchInterval() noexcept : previous_(switch_interval()) {
        if (previous_) {
            set_switch_interval(exit_switch_interval_s);
        }
    }
    ~ShortSwitchInterval() {
        if (previous_) {
            set_switch_interval(*previous_);
        }
    }
    ShortSwitchInterval(const ShortSwitchInterval&) = delete;
    ShortSwitchInterval& operator=(const ShortSwitchInterval&) = delete;
    ShortSwitchInterval(ShortSwitchInterval&&) = delete;
    ShortSwitchInterval& operator=(ShortSwitchInterval&&) = delete;

private:
    static constexpr double exit_switch_interval_s = 50e-6;

    std::optional<double> previous_;  // empty if it could not be read
};

[[nodiscard]] bool no_call_in_python(const ExitState& state) noexcept {
    return state.calls_in_python.load(std::memory_order_seq_cst) == 0;
}

[[nodiscard]] uint64_t queued_records(const Core& c) noexcept {
    const CoreStatistics statistics = c.statistics();
    return statistics.logs.queued + statistics.metrics.queued + statistics.progress.queued;
}

// Steps 2 and 3 of the header. Returns whether no Python callback is running
// any more. Once Python sinks are no longer called, they refuse each record
// at once, so in step 3 the delivery usually finishes quickly: every record
// has then reached each Python sink, received or counted as not.
[[nodiscard]] bool flush_within_limits(ExitState& state, Core& c, ExitOutcome& outcome) {
    const ShortSwitchInterval faster_hand_over;
    WaitResult delivered = flush_until(c, Clock::now() + exit_delivery_limit);
    outcome.interrupted = delivered == WaitResult::Interrupted;
    state.sink_callbacks_open.store(false, std::memory_order_seq_cst);
    const Clock::time_point deadline = Clock::now() + exit_callback_limit;
    if (delivered == WaitResult::TimedOut) {
        delivered = flush_until(c, deadline);
        outcome.interrupted = outcome.interrupted || delivered == WaitResult::Interrupted;
    }
    const WaitResult finished = wait_until([&state] { return no_call_in_python(state); }, deadline);
    outcome.interrupted = outcome.interrupted || finished == WaitResult::Interrupted;
    if (delivered != WaitResult::Done) {
        outcome.queued_at_detach = queued_records(c);
    }
    return no_call_in_python(state);
}

void remove_python_sinks(Core& c, const std::vector<SinkId>& sinks) {
    for (const SinkId id : sinks) {
        std::expected<std::unique_ptr<Sink>, std::error_code> removed;
        {
            // remove_sink waits for a callback of the sink that is running.
            // One that started after step 3 is refused by PythonCall and
            // returns at once, but it must not find the GIL held here.
            const py::gil_scoped_release release;
            removed = c.remove_sink(id);
        }
        // `removed` is destroyed here, with the GIL held. An id that is not
        // registered any more was removed by C++ code; there is nothing left
        // to detach, so the error is not reported.
    }
}

// A call into Python that never returns: see the header. If only sink
// callbacks are stuck, each holds core()'s delivery lock, and its shutdown
// can give up at once instead of waiting for them.
void leave_stuck_calls_behind(ExitState& state, Core& c, ExitOutcome& outcome) {
    outcome.callback_still_running = true;
    outcome.other_call_still_running =
        state.other_calls_in_python.load(std::memory_order_seq_cst) != 0;
    if (!outcome.other_call_still_running) {
        c.set_shutdown_timeout(std::chrono::milliseconds{0});
    }
}

// Steps 1 to 6 of the header. Runs on the thread that finalises the
// interpreter, with the GIL held; in a multiprocessing child, possibly a
// second time, which does nothing.
void detach_from_python() {
    ExitState& state = exit_state();
    std::vector<SinkId> sinks;
    bool reset_diagnostics = false;
    {
        const std::scoped_lock lock(state.mutex);
        if (std::exchange(state.exit_hook_ran, true)) {
            return;
        }
        state.records_open.store(false, std::memory_order_seq_cst);
        sinks = std::exchange(state.python_sinks, {});
        reset_diagnostics = std::exchange(state.python_diagnostics, false);
    }
    Core& c = core();
    ExitOutcome outcome;
    bool idle = flush_within_limits(state, c, outcome);
    const Clock::time_point deadline = Clock::now() + exit_callback_limit;
    if (idle) {
        remove_python_sinks(c, sinks);
    }
    if (reset_diagnostics) {
        const std::scoped_lock lock(state.mutex);
        state.diagnostics = stderr_diagnostics();
        c.set_diagnostic_handler(state.diagnostics);
    }
    state.gate_open.store(false, std::memory_order_seq_cst);
    if (idle) {
        const WaitResult last = wait_until([&state] { return no_call_in_python(state); }, deadline);
        idle = last == WaitResult::Done;
        outcome.interrupted = outcome.interrupted || last == WaitResult::Interrupted;
    }
    if (!idle) {
        leave_stuck_calls_behind(state, c, outcome);
    }
    const std::scoped_lock lock(state.mutex);
    state.outcome = outcome;
}

// The lines of the exit report; none when nothing was lost.
[[nodiscard]] std::vector<std::string> exit_report(const ExitOutcome& outcome, uint64_t refused,
                                                   uint64_t undelivered) {
    std::vector<std::string> lines;
    if (refused != 0) {
        lines.push_back(std::format(
            "lumen: {} record(s) produced from Python after the interpreter began to exit were "
            "dropped",
            refused));
    }
    if (undelivered != 0) {
        lines.push_back(std::format(
            "lumen: {} record(s) were not delivered to Python sinks: they had not taken them {} "
            "ms after the interpreter began to exit{}",
            undelivered, exit_delivery_limit.count(),
            outcome.interrupted ? ", or the wait for them was interrupted" : ""));
    }
    if (outcome.queued_at_detach && !outcome.callback_still_running) {
        lines.push_back(std::format(
            "lumen: the delivery of the records queued at interpreter exit had not finished {} "
            "ms later; Python sinks were detached with {} record(s) still queued, which sinks "
            "written in C++ still receive",
            (exit_delivery_limit + exit_callback_limit).count(), *outcome.queued_at_detach));
    }
    if (outcome.other_call_still_running) {
        lines.push_back(std::format(
            "lumen: a Python diagnostic handler was still running when the interpreter exited; "
            "the process ends now with status {}, without C++ static destruction, because the "
            "library's shutdown could wait for that call forever",
            EXIT_FAILURE));
    } else if (outcome.callback_still_running) {
        lines.emplace_back(
            "lumen: a Python sink was still running when the interpreter exited; the library's "
            "shutdown leaves it and the records queued behind it");
    }
    return lines;
}

// Runs at the very end of Py_FinalizeEx (Py_AtExit), when no Python code can
// run any more: reports what the hook could not deliver, and ends the
// process if a Python call that is not a sink callback never returned.
void report_at_end_of_finalisation() noexcept {
    ExitState& state = exit_state();
    ExitOutcome outcome;
    DiagnosticHandler handler;
    {
        const std::scoped_lock lock(state.mutex);
        outcome = state.outcome;
        handler = state.diagnostics;
    }
    try {
        const std::vector<std::string> lines =
            exit_report(outcome, state.records_refused.load(std::memory_order_relaxed),
                        state.records_undelivered.load(std::memory_order_relaxed));
        if (handler) {
            for (const std::string& line : lines) {
                handler(line);
            }
        }
    } catch (...) {
        // This runs from a C callback, which must not throw; the handler or
        // the formatting failed, so say so where the default handler would.
        std::cerr << "lumen: the report of records lost at interpreter exit could not be "
                     "written\n";
    }
    if (outcome.other_call_still_running) {
        std::cout.flush();
        std::cerr.flush();
        static_cast<void>(std::fflush(nullptr));  // what fails to flush now is lost anyway
        std::_Exit(EXIT_FAILURE);
    }
}

[[nodiscard]] bool kind_admitted(const ExitState& state, PythonCallKind kind) noexcept {
    return state.gate_open.load(std::memory_order_seq_cst) &&
           (kind != PythonCallKind::SinkCallback ||
            state.sink_callbacks_open.load(std::memory_order_seq_cst));
}

void count_call(ExitState& state, PythonCallKind kind, bool entering) noexcept {
    if (entering) {
        state.calls_in_python.fetch_add(1, std::memory_order_seq_cst);
        if (kind == PythonCallKind::Other) {
            state.other_calls_in_python.fetch_add(1, std::memory_order_seq_cst);
        }
        return;
    }
    if (kind == PythonCallKind::Other) {
        state.other_calls_in_python.fetch_sub(1, std::memory_order_seq_cst);
    }
    state.calls_in_python.fetch_sub(1, std::memory_order_seq_cst);
}

// os.register_at_fork hooks, called with the GIL held on the forking thread.
void lock_before_fork() {
    ExitState& state = exit_state();
    state.fork_lock.emplace(state.mutex);
}

void unlock_after_fork_in_parent() {
    exit_state().fork_lock.reset();
}

// In a multiprocessing child (see "fork()" in the header): from inside
// multiprocessing's own after-fork handling, which first clears the
// finalizers, registers the exit hook as one.
void run_hook_at_multiprocessing_exit(const py::object& hook) {
    py::module_::import("multiprocessing.util")
        .attr("Finalize")(py::none(), hook,
                          py::arg("exitpriority") = multiprocessing_exit_priority);
}

void reset_in_fork_child() {
    ExitState& state = exit_state();
    // The calls counted were made by threads of the parent.
    state.calls_in_python.store(0, std::memory_order_seq_cst);
    state.other_calls_in_python.store(0, std::memory_order_seq_cst);
    state.fork_lock.reset();
    const py::dict modules = py::module_::import("sys").attr("modules");
    if (modules.contains("multiprocessing.util")) {
        const py::object hook = py::module_::import(module_name).attr("_detach_from_python");
        modules["multiprocessing.util"].attr("register_after_fork")(
            hook, py::cpp_function(&run_hook_at_multiprocessing_exit));
    }
}

void register_fork_hooks(py::module_& m, const py::object& hook) {
    // multiprocessing's after-fork registry refers to `hook` weakly; the
    // module attribute keeps it alive.
    m.attr("_detach_from_python") = hook;
    py::module_::import("os").attr("register_at_fork")(
        py::arg("before") = py::cpp_function(&lock_before_fork),
        py::arg("after_in_parent") = py::cpp_function(&unlock_after_fork_in_parent),
        py::arg("after_in_child") = py::cpp_function(&reset_in_fork_child));
}

}  // namespace

// Gate protocol (both sides sequentially consistent): a call increments
// calls_in_python and then reads the flags; the exit hook clears a flag and
// then waits for calls_in_python to reach zero. Either the call sees the flag
// cleared, or the hook sees the call and waits for it.
//
// A call that finds its flag already cleared is refused without being
// counted. Calls refused after the counting would keep calls_in_python above
// zero for a moment each, and a stream of them (records still delivered to
// a Python sink that is no longer called) could keep the hook from ever
// seeing zero.
PythonCall::PythonCall(PythonCallKind kind) noexcept : kind_(kind) {
    ExitState& state = exit_state();
    if (!kind_admitted(state, kind)) {
        return;
    }
    count_call(state, kind, true);
    admitted_ = kind_admitted(state, kind) && interpreter_can_run_code();
    if (!admitted_) {
        count_call(state, kind, false);
    }
}

bool PythonCall::still_admitted() const noexcept {
    return admitted_ && kind_admitted(exit_state(), kind_);
}

PythonCall::~PythonCall() {
    if (admitted_) {
        count_call(exit_state(), kind_, false);
    }
}

bool admit_python_record() noexcept {
    ExitState& state = exit_state();
    if (state.records_open.load(std::memory_order_acquire)) {
        return true;
    }
    state.records_refused.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void count_undelivered_python_record() noexcept {
    exit_state().records_undelivered.fetch_add(1, std::memory_order_relaxed);
}

SinkId add_python_sink(Core& core, std::unique_ptr<Sink> sink, Predicate predicate) {
    ExitState& state = exit_state();
    std::expected<SinkId, std::error_code> added;
    bool refused = false;
    {
        // Released before taking the mutex (lock rule above). The sink is
        // never destroyed in here: add_sink only fails for a null sink.
        const py::gil_scoped_release release;
        const std::scoped_lock lock(state.mutex);
        if (state.exit_hook_ran) {
            refused = true;
        } else {
            added = core.add_sink(std::move(sink), std::move(predicate));
            if (added) {
                state.python_sinks.push_back(*added);
            }
        }
    }
    if (refused) {
        raise_after_exit();
    }
    return value_or_raise(std::move(added));
}

void forget_python_sink(SinkId id) noexcept {
    ExitState& state = exit_state();
    const std::scoped_lock lock(state.mutex);
    std::erase(state.python_sinks, id);
}

void set_diagnostic_handler(Core& core, const DiagnosticHandler& handler, bool calls_python) {
    ExitState& state = exit_state();
    const std::scoped_lock lock(state.mutex);
    if (calls_python && state.exit_hook_ran) {
        raise_after_exit();
    }
    core.set_diagnostic_handler(handler);
    state.diagnostics = handler;
    state.python_diagnostics = calls_python;
}

void register_interpreter_exit(py::module_& m) {
    const py::object hook =
        py::cpp_function(&detach_from_python, py::name("_lumen_detach_from_python"));
    py::module_::import("atexit").attr("register")(hook);
    register_fork_hooks(m, hook);
    if (Py_AtExit(&report_at_end_of_finalisation) != 0) {
        PyErr_SetString(PyExc_RuntimeError,
                        "lumen: cannot register its end-of-exit report (Py_AtExit is full)");
        throw py::error_already_set();
    }
}

}  // namespace lumen::python
