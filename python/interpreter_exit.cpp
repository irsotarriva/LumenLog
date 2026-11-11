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
#include "immortal.hpp"
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

// What the exit hook found, for the report at the end of finalisation.
struct ExitOutcome {
    bool interrupted = false;       // a wait ended because of a signal
    bool flush_unfinished = false;  // when the Python sinks were removed
    bool callback_still_running = false;
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
    std::atomic<uint64_t> records_refused{0};
    std::atomic<uint64_t> records_undelivered{0};
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

// Core::flush() on a thread of its own, so that the exit hook can stop
// waiting for it. If the flush has not finished when this is destroyed (a
// callback it delivers to does not return), the thread is detached: joining
// it would wait for that callback.
class BackgroundFlush {
public:
    explicit BackgroundFlush(Core& core) : done_(std::make_shared<std::atomic<bool>>(false)) {
        try {
            // std::thread, not std::jthread: Apple's libc++ lacks it.
            thread_ = std::thread([&core, done = done_] {
                core.flush();
                done->store(true, std::memory_order_release);
            });
        } catch (const std::system_error&) {
            // No thread could be created: flush here, without the time
            // limit, rather than not at all.
            const py::gil_scoped_release release;
            core.flush();
            done_->store(true, std::memory_order_release);
        }
    }
    ~BackgroundFlush() {
        if (!thread_.joinable()) {
            return;
        }
        if (done()) {
            thread_.join();
        } else {
            thread_.detach();
        }
    }
    BackgroundFlush(const BackgroundFlush&) = delete;
    BackgroundFlush& operator=(const BackgroundFlush&) = delete;
    BackgroundFlush(BackgroundFlush&&) = delete;
    BackgroundFlush& operator=(BackgroundFlush&&) = delete;

    [[nodiscard]] bool done() const noexcept { return done_->load(std::memory_order_acquire); }

private:
    std::shared_ptr<std::atomic<bool>> done_;  // shared with the thread, which may outlive this
    std::thread thread_;
};

enum class WaitResult : uint8_t {
    Done,
    TimedOut,
    Interrupted
};

// Waits until `done()` or `deadline`. Called with the GIL held, on the main
// thread (atexit functions run there); `done` is called without the GIL.
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
        if (PyErr_CheckSignals() != 0) {
            // The interpreter is already exiting: the interruption is
            // honoured by waiting no longer, so the exception itself is not
            // needed.
            PyErr_Clear();
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

// Steps 2 and 3 of the header. Returns whether no Python callback is running
// any more. If the flush has not finished by then (a sink written in C++ is
// slow), it goes on on its own thread, and the library's shutdown waits for
// it; but the Python sinks are removed before it reaches the records it has
// not reached yet.
[[nodiscard]] bool flush_within_limits(ExitState& state, Core& c, ExitOutcome& outcome) {
    const ShortSwitchInterval faster_hand_over;
    const BackgroundFlush flush(c);
    const WaitResult delivered =
        wait_until([&flush] { return flush.done(); }, Clock::now() + exit_delivery_limit);
    outcome.interrupted = delivered == WaitResult::Interrupted;
    state.sink_callbacks_open.store(false, std::memory_order_seq_cst);
    const WaitResult finished = wait_until([&] { return flush.done() && no_call_in_python(state); },
                                           Clock::now() + exit_callback_limit);
    outcome.interrupted = outcome.interrupted || finished == WaitResult::Interrupted;
    outcome.flush_unfinished = !flush.done();
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

// Steps 1 to 6 of the header. Runs on the thread that finalises the
// interpreter, with the GIL held.
void detach_from_python() {
    ExitState& state = exit_state();
    std::vector<SinkId> sinks;
    bool reset_diagnostics = false;
    {
        const std::scoped_lock lock(state.mutex);
        state.exit_hook_ran = true;
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
    outcome.callback_still_running = !idle;
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
    if (outcome.flush_unfinished && !outcome.callback_still_running) {
        lines.push_back(std::format(
            "lumen: the delivery of the records queued at interpreter exit had not finished {} "
            "ms later; Python sinks were detached without the records it had not reached (how "
            "many is not known); sinks written in C++ still receive them",
            (exit_delivery_limit + exit_callback_limit).count()));
    }
    if (outcome.callback_still_running) {
        lines.push_back(std::format(
            "lumen: a Python sink or diagnostic handler was still running when the interpreter "
            "exited, and records queued behind it were not delivered; the process ends now with "
            "status {}, without C++ static destruction, because the library's shutdown would "
            "wait for that call forever",
            EXIT_FAILURE));
    }
    return lines;
}

// Runs at the very end of Py_FinalizeEx (Py_AtExit), when no Python code can
// run any more: reports what the exit hook could not deliver, and ends the
// process if a Python callback never returned.
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
    if (outcome.callback_still_running) {
        std::cout.flush();
        std::cerr.flush();
        static_cast<void>(std::fflush(nullptr));  // what fails to flush now is lost anyway
        std::_Exit(EXIT_FAILURE);
    }
}

}  // namespace

// Gate protocol (both sides sequentially consistent): a call increments
// calls_in_python and then reads the flags; the exit hook clears a flag and
// then waits for calls_in_python to reach zero. Either the call sees the flag
// cleared, or the hook sees the call and waits for it.
PythonCall::PythonCall(PythonCallKind kind) noexcept {
    ExitState& state = exit_state();
    state.calls_in_python.fetch_add(1, std::memory_order_seq_cst);
    admitted_ = state.gate_open.load(std::memory_order_seq_cst) &&
                (kind != PythonCallKind::SinkCallback ||
                 state.sink_callbacks_open.load(std::memory_order_seq_cst)) &&
                interpreter_can_run_code();
    if (!admitted_) {
        state.calls_in_python.fetch_sub(1, std::memory_order_seq_cst);
    }
}

PythonCall::~PythonCall() {
    if (admitted_) {
        exit_state().calls_in_python.fetch_sub(1, std::memory_order_seq_cst);
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

void register_interpreter_exit(py::module_& /*m*/) {
    py::module_::import("atexit").attr("register")(
        py::cpp_function(&detach_from_python, py::name("_lumen_detach_from_python")));
    if (Py_AtExit(&report_at_end_of_finalisation) != 0) {
        PyErr_SetString(PyExc_RuntimeError,
                        "lumen: cannot register its end-of-exit report (Py_AtExit is full)");
        throw py::error_already_set();
    }
}

}  // namespace lumen::python
