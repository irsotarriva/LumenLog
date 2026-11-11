#ifndef LUMEN_PYTHON_CALLER_SOURCE_HPP
#define LUMEN_PYTHON_CALLER_SOURCE_HPP

#include <pybind11/pybind11.h>

#include "lumen/record.hpp"

// The source of a record logged from Python. Taken from the Python frame
// that called into the module, through the C API: the frame's code object
// gives the file name and qualified function name (str objects whose UTF-8 is
// cached, so nothing is copied or formatted), and the frame gives the line.
// No Python code runs and nothing is allocated beyond the frame object
// CPython may create on first access. Capture can be turned off
// (set_source_capture), which leaves the source empty.
//
// Python has no innermost enclosing class the way C++ does; class_name is
// the component of the qualified name before the function's own, unless that
// is "<locals>" (a nested function): "Trainer.step" gives "Trainer",
// "main.<locals>.helper" and "helper" give "".

namespace lumen::python {

// A SourceLocation whose views point into Python objects this object holds.
// Valid while it lives and the GIL is held; the record builder copies the
// strings.
class PythonSource {
public:
    // The Python caller of the bound function now running; empty when
    // capture is off or there is no Python frame.
    [[nodiscard]] static PythonSource caller();

    // From a (file, line, function) tuple, as logging.LogRecord provides
    // them. TypeError for anything else.
    [[nodiscard]] static PythonSource given(pybind11::handle source);

    [[nodiscard]] const SourceLocation& location() const noexcept { return location_; }

private:
    PythonSource() = default;

    pybind11::object owner_;  // keeps the strings the views point into alive
    SourceLocation location_{};
};

void set_source_capture(bool enabled) noexcept;
[[nodiscard]] bool source_capture() noexcept;

}  // namespace lumen::python

#endif
