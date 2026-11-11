#include "caller_source.hpp"

#include <pybind11/pybind11.h>

#include <atomic>
#include <cstdint>
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
    source.owner_ = py::reinterpret_steal<py::object>(reinterpret_cast<PyObject*>(code));
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
    const py::tuple parts = py::reinterpret_borrow<py::tuple>(source);
    out.location_.file = utf8(parts[0], "source file");
    out.location_.line = parts[1].cast<uint32_t>();
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
