#ifndef LUMEN_PYTHON_TAG_VALUES_HPP
#define LUMEN_PYTHON_TAG_VALUES_HPP

#include <pybind11/pybind11.h>

#include <concepts>
#include <cstdint>
#include <string_view>
#include <variant>

#include "lumen/detail/string_scratch.hpp"

// Tag keys and values as Python passes them. Keys are str. A value is a str,
// a bool, an int (or any object with __index__, such as a NumPy integer) or a
// float; anything else raises TypeError, and an int outside both the int64
// and the uint64 range raises OverflowError. Numbers are formatted by the
// library exactly as a C++ .tag(key, number) formats them (integers exactly,
// floats in the shortest form that reads back as the same value, bools as
// "true"/"false"; lumen/record_builder.hpp), so a predicate sees the same text
// whichever language logged the record.
//
// Every string_view returned here points into a Python object (or into a
// StringScratch the caller owns) and is valid only while that object lives
// and the GIL is held. None of them is ever stored beyond the call that
// received them: the library copies what it keeps.

namespace lumen::python {

using TagValue = std::variant<std::string_view, int64_t, uint64_t, double, bool>;

// The UTF-8 text of a str; TypeError for anything else. `what` names the
// argument in the error message.
[[nodiscard]] std::string_view utf8(pybind11::handle text, const char* what);

[[nodiscard]] TagValue tag_value(pybind11::handle value);

// The text the library stores for `value`, kept in `scratch` when it had to be
// formatted.
[[nodiscard]] std::string_view tag_text(const TagValue& value, detail::StringScratch& scratch);

// Calls `f(key, value)` for every item of `tags`, which is None (no tags) or a
// dict; TypeError for anything else.
template <std::invocable<std::string_view, const TagValue&> F>
void for_each_tag(pybind11::handle tags, F&& f) {
    if (tags.is_none()) {
        return;
    }
    if (!PyDict_Check(tags.ptr())) {
        throw pybind11::type_error("tags must be a dict or None");
    }
    PyObject* key = nullptr;    // non-owning: borrowed from the dict
    PyObject* value = nullptr;  // non-owning: borrowed from the dict
    Py_ssize_t position = 0;
    while (PyDict_Next(tags.ptr(), &position, &key, &value) != 0) {
        f(utf8(key, "tag key"), tag_value(value));
    }
}

}  // namespace lumen::python

#endif
