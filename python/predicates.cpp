#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lumen/predicate.hpp"
#include "lumen/record.hpp"
#include "registration.hpp"
#include "tag_values.hpp"

namespace py = pybind11;

namespace lumen::python {
namespace {

// Evaluates `p` against tags given as a dict, as Core evaluates it for a
// record with those tags; without a level, as for a metric or progress record.
// The views in record_tags point into `call_tags` and `texts`, which outlive
// it; `texts` is reserved in full, so its strings never move.
[[nodiscard]] bool evaluate(const Predicate& p, py::handle tags, std::optional<LogLevel> level) {
    const CallTags call_tags(tags);
    std::vector<std::string> texts;
    texts.reserve(call_tags.tags().size());
    RecordTags record_tags;
    for (const CallTags::Tag& t : call_tags.tags()) {
        texts.push_back(tag_text(t.value));
        if (!record_tags.set(t.key, texts.back())) {
            throw py::value_error("more tags than a record holds");
        }
    }
    return level ? p.evaluate(record_tags, *level) : p.evaluate(record_tags);
}

[[nodiscard]] Predicate parse(std::string_view query) {
    auto parsed = parse_predicate(query);
    if (!parsed) {
        const PredicateParseError& e = parsed.error();
        const py::object type = py::module_::import(module_name).attr("InvalidPredicateError");
        const std::string text =
            "invalid predicate at offset " + std::to_string(e.position) + ": " + e.message;
        PyErr_SetString(type.ptr(), text.c_str());
        throw py::error_already_set();
    }
    return std::move(*parsed);
}

void register_builders(py::module_& m) {
    m.def("always", &always, "A predicate that matches every record.");
    m.def("never", &never, "A predicate that matches no record.");
    m.def("level_at_least", &level_at_least, py::arg("min_level"),
          "Log records at min_level or above. Ignored for metrics and progress records.");
    m.def("level_equals", &level_equals, py::arg("level"),
          "Log records at exactly this level. Ignored for metrics and progress records.");
    m.def(
        "tag_equals",
        [](std::string_view key, py::handle value) {
            return tag_equals(key, tag_text(tag_value(value)));
        },
        py::arg("key"), py::arg("value"),
        "Records whose tag `key` has exactly this text. A number is compared as the library "
        "formats it (tag_equals('epoch', 3) matches the tag epoch=3). False if the tag is "
        "missing.");
    m.def("tag_exists", &tag_exists, py::arg("key"),
          "Records that have the tag, whatever its value.");
    m.def("tag_less", &tag_less, py::arg("key"), py::arg("value"),
          "Records whose tag `key` parses as a number less than `value`.");
    m.def("tag_less_equal", &tag_less_equal, py::arg("key"), py::arg("value"),
          "Records whose tag `key` parses as a number at most `value`.");
    m.def("tag_greater", &tag_greater, py::arg("key"), py::arg("value"),
          "Records whose tag `key` parses as a number greater than `value`.");
    m.def("tag_greater_equal", &tag_greater_equal, py::arg("key"), py::arg("value"),
          "Records whose tag `key` parses as a number at least `value`.");
    m.def("parse_predicate", &parse, py::arg("query"),
          "Build a Predicate from a query such as \"level >= WARN && run == 42\". Raises "
          "InvalidPredicateError (a ValueError) naming the offset of the problem.");
}

}  // namespace

void register_predicates(py::module_& m) {
    py::class_<Predicate>(m, "Predicate",
                          "Which records a sink receives: an immutable tree of conditions on "
                          "level and tags, combined with &, | and ~. Predicate() matches every "
                          "record.")
        .def(py::init<>())
        .def("evaluate", &evaluate, py::arg("tags"), py::arg("level") = py::none(),
             "Whether a record with these tags (a dict) would pass. With a level, as for a log "
             "record; without one, as for a metric or progress record, where level "
             "conditions are ignored.")
        .def(
            "__and__", [](const Predicate& a, const Predicate& b) { return a && b; },
            py::is_operator())
        .def(
            "__or__", [](const Predicate& a, const Predicate& b) { return a || b; },
            py::is_operator())
        .def("__invert__", [](const Predicate& p) { return !p; });
    register_builders(m);
}

}  // namespace lumen::python
