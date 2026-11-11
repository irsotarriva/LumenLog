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
    // Runs `call` with the GIL held. Does nothing once Python can no longer
    // run code (PythonCall). A Python exception from `call` is thrown on as
    // PythonSinkError, after the GIL has been released.
    template <std::invocable F>
    static void _call_python(F&& call) {
        std::string failure;
        {
            const PythonCall admission;
            if (!admission.admitted()) {
                return;
            }
            const pybind11::gil_scoped_acquire gil;
            try {
                std::forward<F>(call)();
            } catch (const pybind11::error_already_set& e) {
                // Read here: the exception's Python objects need the GIL.
                failure = describe(e);
            }
        }
        if (!failure.empty()) {
            throw PythonSinkError(failure);
        }
    }
};

}  // namespace lumen::python

#endif
