#include "python_diagnostics.hpp"

#include <pybind11/pybind11.h>

#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "interpreter_exit.hpp"
#include "registration.hpp"

namespace py = pybind11;

namespace lumen::python {

struct PythonDiagnostics::Held {
    explicit Held(py::function f) : handler(std::move(f)) {}
    // Released with the GIL; left alone once Python cannot run code.
    ~Held() {
        const PythonCall admission(PythonCallKind::Other);
        if (!admission.admitted()) {
            static_cast<void>(handler.release());
            return;
        }
        try {
            const py::gil_scoped_acquire gil;
            handler = py::function();
        } catch (...) {
            // pybind11 throws when it cannot take the GIL; the callable is
            // then left alone, as after exit.
            static_cast<void>(handler.release());
        }
    }
    Held(const Held&) = delete;
    Held& operator=(const Held&) = delete;
    Held(Held&&) = delete;
    Held& operator=(Held&&) = delete;

    py::function handler;
};

PythonDiagnostics::PythonDiagnostics(py::function handler)
    : held_(std::make_shared<Held>(std::move(handler))) {}

void PythonDiagnostics::operator()(std::string_view line) const {
    const PythonCall admission(PythonCallKind::Other);
    if (!admission.admitted()) {
        std::cerr << line << '\n' << std::flush;
        return;
    }
    const py::gil_scoped_acquire gil;
    try {
        held_->handler(py::str(line.data(), line.size()));
    } catch (py::error_already_set& e) {
        e.discard_as_unraisable("lumen diagnostic handler");
    }
}

py::function PythonDiagnostics::callable() const {
    return held_->handler;
}

py::object stderr_diagnostics_object() {
    return py::module_::import(module_name).attr("stderr_diagnostics");
}

DiagnosticHandler diagnostics_from_python(const py::object& value, bool& calls_python) {
    calls_python = false;
    if (value.is_none()) {
        return {};
    }
    if (value.is(stderr_diagnostics_object())) {
        return stderr_diagnostics();
    }
    if (PyCallable_Check(value.ptr()) == 0) {
        throw py::type_error("a diagnostic handler must be callable or None");
    }
    calls_python = true;
    return PythonDiagnostics(py::reinterpret_borrow<py::function>(value));
}

py::object diagnostics_to_python(const DiagnosticHandler& handler) {
    if (!handler) {
        return py::none();
    }
    if (const auto* const python = handler.target<PythonDiagnostics>(); python != nullptr) {
        return python->callable();
    }
    return stderr_diagnostics_object();
}

void register_diagnostics(py::module_& m) {
    m.def(
        "stderr_diagnostics", [](std::string_view line) { stderr_diagnostics()(line); },
        py::arg("line"),
        "The library's default diagnostic handler: writes the line to the process's standard "
        "error. Give it where a handler is expected to get the default back.");
}

}  // namespace lumen::python
