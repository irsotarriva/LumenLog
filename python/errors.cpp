#include "errors.hpp"

#include <pybind11/pybind11.h>

#include <string>
#include <string_view>
#include <system_error>

#include "lumen/error.hpp"
#include "registration.hpp"

namespace py = pybind11;

namespace lumen::python {
namespace {

// Creates an exception class in `m` and returns it (a new reference held by `m`).
py::object add_exception(py::module_& m, const char* name, const char* doc, py::handle bases) {
    const std::string qualified = std::string(module_name) + "." + name;
    py::object type = py::reinterpret_steal<py::object>(
        PyErr_NewExceptionWithDoc(qualified.c_str(), doc, bases.ptr(), nullptr));
    if (!type) {
        throw py::error_already_set();
    }
    m.attr(name) = type;
    return type;
}

[[nodiscard]] const char* exception_name(LumenError e) {
    switch (e) {
        case LumenError::InvalidSinkId:     return "InvalidSinkIdError";
        case LumenError::SinkBusy:          return "SinkBusyError";
        case LumenError::TagTableFull:      return "TagTableFullError";
        case LumenError::InvalidPredicate:  return "InvalidPredicateError";
        case LumenError::InvalidSinkConfig: return "InvalidSinkConfigError";
        case LumenError::FileOpenFailed:    return "FileOpenError";
        case LumenError::None:
        case LumenError::NullSink:
        case LumenError::IoError:
        case LumenError::NotATerminal:
        case LumenError::CoreShutDown:      return "LumenError";
    }
    return "LumenError";
}

}  // namespace

void register_errors(py::module_& m) {
    const py::object base =
        add_exception(m, "LumenError",
                      "Base class of the errors the library reports. Raised as is for an error "
                      "that has no more specific class.",
                      PyExc_RuntimeError);
    add_exception(m, "InvalidSinkIdError",
                  "No sink is registered under this id (Core.remove_sink, "
                  "Core.sink_exception_count).",
                  py::make_tuple(base, py::handle(PyExc_LookupError)));
    add_exception(m, "SinkBusyError",
                  "Core.remove_sink was called from inside a callback of the sink being removed.",
                  py::make_tuple(base));
    add_exception(m, "TagTableFullError",
                  "set_thread_tag or set_process_tag with a new key on a table that already "
                  "holds its maximum number of keys; the table is unchanged.",
                  py::make_tuple(base));
    add_exception(m, "InvalidPredicateError",
                  "parse_predicate was given a query that is not valid.",
                  py::make_tuple(base, py::handle(PyExc_ValueError)));
    add_exception(m, "InvalidSinkConfigError",
                  "A sink was created with a configuration field outside its documented range.",
                  py::make_tuple(base, py::handle(PyExc_ValueError)));
    add_exception(m, "FileOpenError", "A FileSink could not open its file for appending.",
                  py::make_tuple(base, py::handle(PyExc_OSError)));
}

void raise_error(std::error_code code) {
    raise_error(code, {});
}

void raise_error(std::error_code code, std::string_view context) {
    if (code.category() == std::generic_category() || code.category() == std::system_category()) {
        // OSError(errno, text) picks the matching subclass, e.g. FileNotFoundError.
        PyErr_SetObject(PyExc_OSError, py::make_tuple(code.value(), code.message()).ptr());
        throw py::error_already_set();
    }
    const char* name = "LumenError";
    if (code.category() == lumen_error_category()) {
        name = exception_name(static_cast<LumenError>(code.value()));
    }
    const py::object type = py::module_::import(module_name).attr(name);
    std::string text = code.message();
    if (!context.empty()) {
        text += ": ";
        text += context;
    }
    PyErr_SetString(type.ptr(), text.c_str());
    throw py::error_already_set();
}

}  // namespace lumen::python
