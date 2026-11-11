#ifndef LUMEN_PYTHON_PYTHON_DIAGNOSTICS_HPP
#define LUMEN_PYTHON_PYTHON_DIAGNOSTICS_HPP

#include <pybind11/pybind11.h>

#include <memory>
#include <string_view>

#include "lumen/diagnostics.hpp"

// Diagnostic handlers given from Python: for the Core
// (Core.set_diagnostic_handler) and for the shipped sinks (the `diagnostics`
// field of TerminalConfig and FileConfig).

namespace lumen::python {

// A Python callable as a DiagnosticHandler. The library copies and calls
// handlers on its own threads without the GIL, so the callable is held
// through a shared_ptr: copies change no Python reference count.
//
// A call takes the GIL. An exception the callable raises has no caller to go
// to; it is passed to sys.unraisablehook. Once the exit hook has closed its
// gate, or Python can no longer run code (python/interpreter_exit.hpp), the
// line goes to std::cerr instead, and the callable is released without the
// GIL (left alone) when the last copy goes.
class PythonDiagnostics {
public:
    explicit PythonDiagnostics(pybind11::function handler);

    void operator()(std::string_view line) const;

    // The callable. GIL held.
    [[nodiscard]] pybind11::function callable() const;

private:
    struct Held;
    std::shared_ptr<Held> held_;
};

// The Python object that stands for stderr_diagnostics() (the module
// attribute `stderr_diagnostics`). Calling it writes the line to std::cerr.
[[nodiscard]] pybind11::object stderr_diagnostics_object();

// The DiagnosticHandler for a value given from Python: None discards the
// lines, stderr_diagnostics_object() is stderr_diagnostics(), and any other
// callable becomes a PythonDiagnostics. Raises TypeError for a value that is
// not callable. Sets `calls_python` if the handler runs Python code. GIL held.
[[nodiscard]] DiagnosticHandler diagnostics_from_python(const pybind11::object& value,
                                                        bool& calls_python);

// The inverse, for reading a configuration's field back: the callable of a
// PythonDiagnostics, None for an empty handler, and
// stderr_diagnostics_object() for any other. GIL held.
[[nodiscard]] pybind11::object diagnostics_to_python(const DiagnosticHandler& handler);

}  // namespace lumen::python

#endif
