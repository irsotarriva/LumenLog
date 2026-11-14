#include "tag_values.hpp"

#include <pybind11/pybind11.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

#include "lumen/detail/string_scratch.hpp"

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
        PyErr_Clear();  // replaced by the OverflowError below, which names both ranges
    }
    PyErr_SetString(PyExc_OverflowError, "integer tag value does not fit in int64 or uint64");
    throw py::error_already_set();
}

// Whether tag_value() reads `value` through a Python method (__index__).
// The built-in types are read through the C API, which runs no Python code.
[[nodiscard]] bool read_by_python_code(py::handle value) {
    PyObject* const object = value.ptr();  // non-owning
    return !PyUnicode_Check(object) && !PyFloat_Check(object) && !PyLong_Check(object);
}

void raise_changed_while_read() {
    PyErr_SetString(PyExc_RuntimeError, "the tags dict changed while it was read");
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
    if (PyIndex_Check(object) != 0) {
        const auto index = py::reinterpret_steal<py::object>(PyNumber_Index(object));
        if (!index) {
            throw py::error_already_set();
        }
        return integer_value(index);
    }
    throw py::type_error("tag value must be str, bool, int or float, not " +
                         std::string(py::str(py::type::handle_of(value).attr("__name__"))));
}

std::string tag_text(const TagValue& value) {
    // StringScratch::store_number is the formatting RecordBuilder::tag uses.
    struct Visitor {
        detail::StringScratch& scratch;
        std::string operator()(std::string_view text) const { return std::string(text); }
        std::string operator()(bool flag) const { return flag ? "true" : "false"; }
        std::string operator()(int64_t n) const { return std::string(scratch.store_number(n)); }
        std::string operator()(uint64_t n) const { return std::string(scratch.store_number(n)); }
        std::string operator()(double x) const { return std::string(scratch.store_number(x)); }
    };
    detail::StringScratch scratch;
    return std::visit(Visitor{scratch}, value);
}

CallTags::CallTags(py::handle tags) {
    if (tags.is_none()) {
        return;
    }
    if (!PyDict_Check(tags.ptr())) {
        throw py::type_error("tags must be a dict or None");
    }
    take_items_(tags);
    bool python_code_ran = false;
    for (std::size_t i = 0; i < owners_.size(); i += 2) {
        python_code_ran = python_code_ran || read_by_python_code(owners_[i + 1]);
        tags_.push_back(Tag{utf8(owners_[i], "tag key"), tag_value(owners_[i + 1])});
    }
    if (python_code_ran) {
        check_unchanged_(tags);
    }
}

void CallTags::take_items_(py::handle dict) {
    const auto size = static_cast<std::size_t>(PyDict_Size(dict.ptr()));
    owners_.reserve(2 * size);
    tags_.reserve(size);
    // Nothing in this loop runs Python code, so the dict cannot change while
    // its items are taken.
    PyObject* key = nullptr;    // non-owning: borrowed from the dict
    PyObject* value = nullptr;  // non-owning: borrowed from the dict
    Py_ssize_t position = 0;
    while (PyDict_Next(dict.ptr(), &position, &key, &value) != 0) {
        owners_.push_back(py::reinterpret_borrow<py::object>(key));
        owners_.push_back(py::reinterpret_borrow<py::object>(value));
    }
}

// Every key must still map to the value that was read. The lookups may
// themselves run Python code (a str subclass with its own __eq__); the views
// stay valid regardless, since this object holds what they point into.
void CallTags::check_unchanged_(py::handle dict) const {
    if (static_cast<std::size_t>(PyDict_Size(dict.ptr())) != tags_.size()) {
        raise_changed_while_read();
    }
    for (std::size_t i = 0; i < owners_.size(); i += 2) {
        PyObject* const now = PyDict_GetItemWithError(dict.ptr(), owners_[i].ptr());  // non-owning
        if (now == nullptr && PyErr_Occurred() != nullptr) {
            throw py::error_already_set();
        }
        if (now != owners_[i + 1].ptr()) {
            raise_changed_while_read();
        }
    }
}

}  // namespace lumen::python
