#include <pybind11/native_enum.h>
#include <pybind11/pybind11.h>

#include "lumen/level.hpp"
#include "lumen/record.hpp"
#include "registration.hpp"

namespace py = pybind11;

namespace lumen::python {
namespace {

[[nodiscard]] py::dict tags_dict(const RecordTags& tags) {
    py::dict out;
    for (const RecordTags::Entry& e : tags) {
        out[py::str(e.key.data(), e.key.size())] = py::str(e.value.data(), e.value.size());
    }
    return out;
}

void register_level(py::module_& m) {
    // An IntEnum: the numeric values are part of the library's interface
    // (lumen/level.hpp), so LogLevel.ERROR == 4 and levels order like numbers.
    py::native_enum<LogLevel>(m, "LogLevel", "enum.IntEnum",
                              "Severity of a log record, in increasing order (TRACE = 0 ... "
                              "FATAL = 5). An IntEnum: members compare equal to their numbers.")
        .value("TRACE", LogLevel::Trace)
        .value("DEBUG", LogLevel::Debug)
        .value("INFO", LogLevel::Info)
        .value("WARN", LogLevel::Warn)
        .value("ERROR", LogLevel::Error)
        .value("FATAL", LogLevel::Fatal)
        .finalize();
}

void register_source(py::module_& m) {
    py::class_<SourceLocation>(m, "SourceLocation",
                               "Where a log record was produced. For a record logged from Python: "
                               "the calling file, line and qualified function name, and the "
                               "innermost enclosing class (empty outside a class); all empty and 0 "
                               "when source capture is off (set_source_capture).")
        .def_readonly("file", &SourceLocation::file)
        .def_readonly("function", &SourceLocation::function)
        .def_readonly("class_name", &SourceLocation::class_name)
        .def_readonly("line", &SourceLocation::line);
}

}  // namespace

void register_records(py::module_& m) {
    register_level(m);
    register_source(m);

    // Records reach Python only as arguments of sink callbacks, as copies
    // that share the record's string storage, so a sink may keep them. They
    // are read-only: nothing in Python can point a record at Python memory.
    // `tags` is a new dict on every access.
    py::class_<LogRecord>(m, "LogRecord", "A log record, as a sink receives it. Read-only.")
        .def_readonly("level", &LogRecord::level)
        .def_readonly("message", &LogRecord::message)
        .def_property_readonly("tags", [](const LogRecord& r) { return tags_dict(r.tags); })
        .def_readonly("timestamp_ns", &LogRecord::timestamp_ns)
        .def_readonly("thread_id", &LogRecord::thread_id)
        .def_readonly("source", &LogRecord::source);

    py::class_<MetricRecord>(m, "MetricRecord",
                             "A metric record, as a sink receives it. Read-only.")
        .def_readonly("name", &MetricRecord::name)
        .def_readonly("value", &MetricRecord::value)
        .def_property_readonly("tags", [](const MetricRecord& r) { return tags_dict(r.tags); })
        .def_readonly("timestamp_ns", &MetricRecord::timestamp_ns)
        .def_readonly("thread_id", &MetricRecord::thread_id);

    py::class_<ProgressRecord>(m, "ProgressRecord",
                               "One state of a progress bar, as a sink receives it. Read-only. "
                               "Every record of one bar has the same id.")
        .def_readonly("id", &ProgressRecord::id)
        .def_readonly("label", &ProgressRecord::label)
        .def_readonly("current", &ProgressRecord::current)
        .def_readonly("total", &ProgressRecord::total)
        .def_property_readonly("tags", [](const ProgressRecord& r) { return tags_dict(r.tags); })
        .def_readonly("timestamp_ns", &ProgressRecord::timestamp_ns)
        .def_readonly("thread_id", &ProgressRecord::thread_id);
}

}  // namespace lumen::python
