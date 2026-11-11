#include "caller_source.hpp"

#include <pybind11/pybind11.h>

#include <atomic>
#include <cstdint>
#include <limits>
#include <string_view>

#include "tag_values.hpp"

namespace py = pybind11;

namespace lumen::python {
namespace {

// The module-wide switch of set_source_capture(); this function is its only owner.
[[nodiscard]] std::atomic<bool>& capture_enabled() {
    static std::atomic<bool> enabled{true};
    return enabled;
}

[[nodiscard]] std::string_view class_of(std::string_view qualname) {
    const std::size_t last = qualname.rfind('.');
    if (last == std::string_view::npos) {
        return {};
    }
    const std::string_view enclosing = qualname.substr(0, last);
    const std::size_t before = enclosing.rfind('.');
    const std::string_view name =
        before == std::string_view::npos ? enclosing : enclosing.substr(before + 1);
    return name == "<locals>" ? std::string_view{} : name;
}

// The line of a given source: an int (or an object with __index__) in
// [0, 2**32). Anything else raises TypeError, as an int argument that does not
// fit its parameter does everywhere in the module (pybind11's rule).
[[nodiscard]] uint32_t line_number(py::handle line) {
    constexpr const char* message = "source line must be an int in [0, 2**32)";
    if (PyIndex_Check(line.ptr()) == 0) {
        throw py::type_error(message);
    }
    const auto index = py::reinterpret_steal<py::object>(PyNumber_Index(line.ptr()));
    if (!index) {
        throw py::error_already_set();
    }
    const unsigned long long value = PyLong_AsUnsignedLongLong(index.ptr());
    if (PyErr_Occurred() != nullptr) {
        PyErr_Clear();  // a negative or too large int: replaced by the TypeError below
        throw py::type_error(message);
    }
    if (value > std::numeric_limits<uint32_t>::max()) {
        throw py::type_error(message);
    }
    return static_cast<uint32_t>(value);
}

}  // namespace

PythonSource PythonSource::caller() {
    PythonSource source;
    if (!source_capture()) {
        return source;
    }
    PyFrameObject* const frame = PyEval_GetFrame();  // non-owning: borrowed
    if (frame == nullptr) {
        return source;
    }
    PyCodeObject* const code = PyFrame_GetCode(frame);  // a new reference, owned below
    // ob_base.ob_base is the code object's PyObject header (PyObject_VAR_HEAD).
    source.owner_ = py::reinterpret_steal<py::object>(&code->ob_base.ob_base);
    const std::string_view qualname = utf8(code->co_qualname, "co_qualname");
    source.location_.file = utf8(code->co_filename, "co_filename");
    source.location_.function = qualname;
    source.location_.class_name = class_of(qualname);
    source.location_.line = static_cast<uint32_t>(PyFrame_GetLineNumber(frame));
    return source;
}

PythonSource PythonSource::given(py::handle source) {
    if (!PyTuple_Check(source.ptr()) || PyTuple_GET_SIZE(source.ptr()) != 3) {
        throw py::type_error("source must be a (file, line, function) tuple");
    }
    PythonSource out;
    out.owner_ = py::reinterpret_borrow<py::object>(source);
    const auto parts = py::reinterpret_borrow<py::tuple>(source);
    out.location_.file = utf8(parts[0], "source file");
    out.location_.line = line_number(parts[1]);
    out.location_.function = utf8(parts[2], "source function");
    return out;
}

void set_source_capture(bool enabled) noexcept {
    capture_enabled().store(enabled, std::memory_order_relaxed);
}

bool source_capture() noexcept {
    return capture_enabled().load(std::memory_order_relaxed);
}

}  // namespace lumen::python
