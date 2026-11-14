#ifndef LUMEN_PYTHON_TAG_VALUES_HPP
#define LUMEN_PYTHON_TAG_VALUES_HPP

#include <pybind11/pybind11.h>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// Tag keys and values as Python passes them. Keys are str. A value is a str,
// a bool, an int (or any object with __index__, such as a NumPy integer) or a
// float; anything else raises TypeError, and an int outside both the int64
// and the uint64 range raises OverflowError. Numbers are formatted by the
// library exactly as a C++ .tag(key, number) formats them (integers exactly,
// floats in the shortest form that reads back as the same value, bools as
// "true"/"false"; lumen/record_builder.hpp), so a predicate sees the same text
// whichever language logged the record.
//
// Every string_view here points into a str object and is valid only while
// that object lives and the GIL is held. A str never changes its text, and
// its UTF-8 form, once made, lives as long as the str.

namespace lumen::python {

using TagValue = std::variant<std::string_view, int64_t, uint64_t, double, bool>;

// The UTF-8 text of a str; TypeError for anything else. `what` names the
// argument in the error message.
[[nodiscard]] std::string_view utf8(pybind11::handle text, const char* what);

// A str value is a view into `value`. An object with __index__ (not an int)
// runs its Python method, which may run any Python code.
[[nodiscard]] TagValue tag_value(pybind11::handle value);

// The text the library stores for `value`.
[[nodiscard]] std::string tag_text(const TagValue& value);

// The tags argument of one call (None or a dict), read and checked in full
// before anything uses them: a record builder submits its record when it is
// destroyed, also when a tag raised half way.
//
// Reading a value can run Python code (__index__), and that code can change
// or empty the dict. So the dict's items are first taken with a strong
// reference each, which runs no Python code; the views in tags() point into
// those objects and stay valid however the dict changes. A dict that changed
// while it was read raises RuntimeError, as iterating over it in Python would.
// TypeError and OverflowError as above; TypeError for tags that are neither
// None nor a dict. Construct and destroy it with the GIL held.
class CallTags {
public:
    struct Tag {
        std::string_view key;
        TagValue value;
    };

    explicit CallTags(pybind11::handle tags);

    [[nodiscard]] std::span<const Tag> tags() const noexcept { return tags_; }

private:
    void take_items_(pybind11::handle dict);
    void check_unchanged_(pybind11::handle dict) const;

    // Every key and value the views point into: key, value, key, value, ...
    std::vector<pybind11::object> owners_;
    std::vector<Tag> tags_;
};

}  // namespace lumen::python

#endif
