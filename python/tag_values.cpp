#include "tag_values.hpp"

#include <pybind11/pybind11.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace py = pybind11;

namespace lumen::python {
namespace {

// `number` is a Python int. The C API's own types (long long) are used at
// the call and converted at once.
[[nodiscard]] TagValue integer_value(py::handle number) {
    int overflow = 0;
    const long long as_signed = PyLong_AsLongLongAndOverflow(number.ptr(), &overflow);
    if (overflow == 0) {
        if (as_signed == -1 && PyErr_Occurred() != nullptr) {
            throw py::error_already_set();
        }
        return static_cast<int64_t>(as_signed);
    }
    if (overflow > 0) {
        const unsigned long long as_unsigned = PyLong_AsUnsignedLongLong(number.ptr());
        if (PyErr_Occurred() == nullptr) {
            return static_cast<uint64_t>(as_unsigned);
        }
        PyErr_Clear();
    }
    PyErr_SetString(PyExc_OverflowError, "integer tag value does not fit in int64 or uint64");
    throw py::error_already_set();
}

}  // namespace

std::string_view utf8(py::handle text, const char* what) {
    if (!PyUnicode_Check(text.ptr())) {
        throw py::type_error(std::string(what) + " must be a str, not " +
                             std::string(py::str(py::type::handle_of(text).attr("__name__"))));
    }
    Py_ssize_t size = 0;
    // The UTF-8 form is cached in the str object, so this copies nothing and
    // the view lives as long as the str.
    const char* data = PyUnicode_AsUTF8AndSize(text.ptr(), &size);
    if (data == nullptr) {
        throw py::error_already_set();
    }
    return {data, static_cast<std::size_t>(size)};
}

TagValue tag_value(py::handle value) {
    PyObject* const object = value.ptr();  // non-owning
    if (PyUnicode_Check(object)) {
        return utf8(value, "tag value");
    }
    if (PyBool_Check(object)) {
        return object == Py_True;
    }
    if (PyFloat_Check(object)) {
        return PyFloat_AS_DOUBLE(object);
    }
    if (PyLong_Check(object)) {
        return integer_value(value);
    }
    if (PyIndex_Check(object)) {
        const py::object index = py::reinterpret_steal<py::object>(PyNumber_Index(object));
        if (!index) {
            throw py::error_already_set();
        }
        return integer_value(index);
    }
    throw py::type_error("tag value must be str, bool, int or float, not " +
                         std::string(py::str(py::type::handle_of(value).attr("__name__"))));
}

std::string_view tag_text(const TagValue& value, detail::StringScratch& scratch) {
    struct Visitor {
        detail::StringScratch& scratch;
        std::string_view operator()(std::string_view text) const { return text; }
        std::string_view operator()(bool flag) const { return flag ? "true" : "false"; }
        std::string_view operator()(int64_t n) const { return scratch.store_number(n); }
        std::string_view operator()(uint64_t n) const { return scratch.store_number(n); }
        std::string_view operator()(double x) const { return scratch.store_number(x); }
    };
    return std::visit(Visitor{scratch}, value);
}

}  // namespace lumen::python
