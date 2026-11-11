#ifndef LUMEN_PYTHON_PYTHON_SINKS_HPP
#define LUMEN_PYTHON_PYTHON_SINKS_HPP

#include <pybind11/pybind11.h>

#include <concepts>
#include <stdexcept>
#include <string>
#include <utility>

#include "interpreter_exit.hpp"
#include "lumen/sink.hpp"

// Sinks whose callbacks run Python code: Python subclasses of lumen.Sink
// (through SinkTrampoline) and CallbackSink. Core calls them on its dispatch
// thread, or on the thread that calls flush(); each callback takes the GIL
// for the duration of the Python call.

namespace lumen::python {

// What a Python sink callback throws to Core when the Python code raised:
// Core counts it (Core.sink_exception_count) and reports the first one per
// sink through the diagnostic handler. The Python exception itself is cleared.
class PythonSinkError : public std::runtime_error {
public:
    explicit PythonSinkError(const std::string& what) : std::runtime_error(what) {}
};

// "TypeName: message" for the Python exception in `e`. GIL held.
[[nodiscard]] std::string describe(const pybind11::error_already_set& e);

class PythonSink : public Sink {
protected:
    // Runs `call` with the GIL held, unless Python sinks are no longer called
    // or Python can no longer run code (PythonCall); returns whether it ran.
    // A Python exception from `call` is thrown on as PythonSinkError, after
    // the GIL has been released.
    template <std::invocable F>
    static bool _call_python(F call) {
        std::string failure;
        {
            const PythonCall admission(PythonCallKind::SinkCallback);
            if (!admission.admitted()) {
                return false;
            }
            const pybind11::gil_scoped_acquire gil;
            try {
                call();
            } catch (const pybind11::error_already_set& e) {
                // Read here: the exception's Python objects need the GIL.
                failure = describe(e);
            }
        }
        if (!failure.empty()) {
            throw PythonSinkError(failure);
        }
        return true;
    }

    // _call_python for a record callback: a record the sink does not receive
    // is counted for the exit report (python/interpreter_exit.hpp).
    template <std::invocable F>
    static void _deliver(F call) {
        if (!_call_python(std::move(call))) {
            count_undelivered_python_record();
        }
    }
};

}  // namespace lumen::python

#endif
