#include "interpreter_exit.hpp"

#include <pybind11/pybind11.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "errors.hpp"
#include "immortal.hpp"
#include "registration.hpp"

namespace py = pybind11;

namespace lumen::python {
namespace {

// Everything the exit hook coordinates. One instance for the process, never
// destroyed (a C++ thread may still construct a PythonCall during static
// destruction; it must find the gate closed, not a destroyed object).
//
// Lock rule: a thread holding `mutex` never waits for the GIL. Threads that
// take it with the GIL held only do work that cannot wait for the GIL either
// (the GIL is re-entrant for them).
struct ExitState {
    std::mutex mutex;
    bool exit_hook_ran = false;         // guarded by mutex
    std::vector<SinkId> python_sinks;   // guarded by mutex
    bool python_diagnostics = false;    // guarded by mutex
    std::atomic<bool> gate_open{true};  // see PythonCall
    std::atomic<uint64_t> calls_in_python{0};
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

// Steps 1 to 5 of the header. Runs on the thread that finalises the
// interpreter, with the GIL held.
void detach_from_python() {
    ExitState& state = exit_state();
    std::vector<SinkId> sinks;
    bool reset_diagnostics = false;
    {
        const std::scoped_lock lock(state.mutex);
        state.exit_hook_ran = true;
        sinks = std::exchange(state.python_sinks, {});
        reset_diagnostics = std::exchange(state.python_diagnostics, false);
    }
    Core& c = core();
    {
        // Python sinks need the GIL to receive what is delivered here.
        const py::gil_scoped_release release;
        c.flush();
    }
    for (const SinkId id : sinks) {
        std::expected<std::unique_ptr<Sink>, std::error_code> removed;
        {
            // remove_sink waits for a running callback, which needs the GIL.
            const py::gil_scoped_release release;
            removed = c.remove_sink(id);
        }
        // `removed` is destroyed here, with the GIL held. An id that is not
        // registered any more was removed by C++ code; there is nothing left
        // to detach, so the error is not reported.
    }
    if (reset_diagnostics) {
        c.set_diagnostic_handler(stderr_diagnostics());
    }
    state.gate_open.store(false, std::memory_order_seq_cst);
    const py::gil_scoped_release release;
    while (state.calls_in_python.load(std::memory_order_seq_cst) != 0) {
        std::this_thread::yield();
    }
}

}  // namespace

// Gate protocol (both sides sequentially consistent): a call increments
// calls_in_python and then reads gate_open; the exit hook clears gate_open and
// then waits for calls_in_python to reach zero. Either the call sees the gate
// closed, or the hook sees the call and waits for it.
PythonCall::PythonCall() noexcept {
    ExitState& state = exit_state();
    state.calls_in_python.fetch_add(1, std::memory_order_seq_cst);
    admitted_ = state.gate_open.load(std::memory_order_seq_cst) && interpreter_can_run_code();
    if (!admitted_) {
        state.calls_in_python.fetch_sub(1, std::memory_order_seq_cst);
    }
}

PythonCall::~PythonCall() {
    if (admitted_) {
        exit_state().calls_in_python.fetch_sub(1, std::memory_order_seq_cst);
    }
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

void set_diagnostic_handler(Core& core, DiagnosticHandler handler, bool calls_python) {
    ExitState& state = exit_state();
    const std::scoped_lock lock(state.mutex);
    if (calls_python && state.exit_hook_ran) {
        raise_after_exit();
    }
    core.set_diagnostic_handler(std::move(handler));
    state.python_diagnostics = calls_python;
}

void register_interpreter_exit(py::module_& /*m*/) {
    py::module_::import("atexit").attr("register")(
        py::cpp_function(&detach_from_python, py::name("_lumen_detach_from_python")));
}

}  // namespace lumen::python
