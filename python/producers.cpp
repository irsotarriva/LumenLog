#include <pybind11/pybind11.h>

#include <array>
#include <cstdint>
#include <exception>
#include <optional>
#include <string_view>
#include <variant>

#include "caller_source.hpp"
#include "interpreter_exit.hpp"
#include "lumen/core.hpp"
#include "lumen/level.hpp"
#include "lumen/progress.hpp"
#include "lumen/record_builder.hpp"
#include "lumen/thread_clock.hpp"
#include "registration.hpp"
#include "tag_values.hpp"

namespace py = pybind11;

namespace lumen::python {
namespace {

// The producers keep the GIL: submitting a record takes no lock that the
// dispatch thread holds while it waits for the GIL, and never waits for a
// sink. fatal() is the exception (see there).
//
// Once the interpreter has begun to exit, the producers drop their record
// (admit_python_record, python/interpreter_exit.hpp); their arguments are
// still checked first, so a wrong one raises as before.

template <BuiltRecord R>
void add_tags(BasicRecordBuilder<R>& builder, const CallTags& tags) {
    for (const CallTags::Tag& t : tags.tags()) {
        std::visit([&builder, &t](auto value) { builder.tag(t.key, value); }, t.value);
    }
}

[[nodiscard]] PythonSource source_of(py::handle source) {
    return source.is_none() ? PythonSource::caller() : PythonSource::given(source);
}

void log_at(LogLevel level, const py::str& message, py::handle tags, py::handle source) {
    if (level == LogLevel::Fatal) {
        throw py::value_error(
            "log() does not take FATAL, which terminates the process; call "
            "fatal() for that");
    }
    const std::string_view text = utf8(message, "message");
    const CallTags call_tags(tags);
    const PythonSource where = source_of(source);
    if (!admit_python_record()) {
        return;
    }
    RecordBuilder builder(core(), level, where.location(), text);
    add_tags(builder, call_tags);
}

[[noreturn]] void fatal(const py::str& message, py::handle tags) {
    const std::string_view text = utf8(message, "message");
    const CallTags call_tags(tags);
    const PythonSource where = PythonSource::caller();
    // Built with the GIL (its strings are read from Python objects), destroyed
    // without it: the destructor delivers the record to every sink, Python
    // sinks included, which need the GIL, then terminates.
    std::optional<RecordBuilder> builder;
    builder.emplace(core(), LogLevel::Fatal, where.location(), text);
    add_tags(*builder, call_tags);
    const py::gil_scoped_release release;
    builder.reset();
    std::terminate();  // not reached: the builder terminates
}

void metric(const py::str& name, double value, py::handle tags) {
    const std::string_view text = utf8(name, "name");
    const CallTags call_tags(tags);
    if (!admit_python_record()) {
        return;
    }
    MetricBuilder builder(core(), text, value);
    add_tags(builder, call_tags);
}

struct LevelFunction {
    const char* name;
    LogLevel level;
    const char* doc;
};

constexpr std::array level_functions{
    LevelFunction{"trace", LogLevel::Trace, "Log `message` at TRACE. See log() for `tags`."},
    LevelFunction{"debug", LogLevel::Debug, "Log `message` at DEBUG. See log() for `tags`."},
    LevelFunction{"info", LogLevel::Info, "Log `message` at INFO. See log() for `tags`."},
    LevelFunction{"warn", LogLevel::Warn, "Log `message` at WARN. See log() for `tags`."},
    LevelFunction{"error", LogLevel::Error, "Log `message` at ERROR. See log() for `tags`."},
};

void register_logging(py::module_& m) {
    m.def("log", &log_at, py::arg("level"), py::arg("message"), py::kw_only(),
          py::arg("tags") = py::none(), py::arg("source") = py::none(),
          "Log `message` verbatim at `level` (TRACE to ERROR; FATAL raises ValueError, see "
          "fatal()).\n\n"
          "tags: None or a dict of str keys to str, bool, int or float values, which become "
          "the record's explicit tags (numbers are formatted as the C++ API formats them).\n"
          "source: None to record the calling Python file, line and function (unless "
          "set_source_capture(False)), or a (file, line, function) tuple to record instead.");
    for (const LevelFunction& f : level_functions) {
        m.def(
            f.name,
            [level = f.level](const py::str& message, py::handle tags) {
                log_at(level, message, tags, py::none());
            },
            py::arg("message"), py::kw_only(), py::arg("tags") = py::none(), f.doc);
    }
    m.def("fatal", &fatal, py::arg("message"), py::kw_only(), py::arg("tags") = py::none(),
          "Log `message` at FATAL and terminate the process: every record queued before it "
          "and then this one are delivered to every sink, the sinks are flushed, and the "
          "process ends with SIGABRT (std::abort(), or std::terminate() when a C++ terminate "
          "handler other than the default is installed). Never returns; Python cleanup "
          "(finally blocks, atexit) does not run.");
    m.def("set_source_capture", &set_source_capture, py::arg("enabled"),
          "Turn recording of the calling file, line and function on (the default) or off. "
          "When off, records logged from Python have an empty source.");
    m.def("source_capture", &source_capture,
          "Whether records logged from Python record their caller.");
}

void register_metric_and_progress(py::module_& m) {
    m.def("metric", &metric, py::arg("name"), py::arg("value"), py::kw_only(),
          py::arg("tags") = py::none(), "Record a metric. `tags` as for log().");

    py::class_<ProgressHandle>(m, "Progress",
                               "A progress bar: a sequence of ProgressRecords with one id. "
                               "update(n) sends n/total, tick() one more; a state equal to the "
                               "last one sent is not sent again. finish() sends total/total "
                               "and ends the bar. A bar dropped unfinished sends nothing more. "
                               "Not for concurrent use from several threads.")
        .def(
            "update",
            [](ProgressHandle& handle, uint64_t current) {
                if (admit_python_record()) {
                    handle.update(current);
                }
            },
            py::arg("current"),
            "Send current/total, unless it is the state sent last or the bar is finished.")
        .def(
            "tick",
            [](ProgressHandle& handle) {
                if (admit_python_record()) {
                    handle.tick();
                }
            },
            "update(current + 1).")
        .def(
            "finish",
            [](ProgressHandle& handle) {
                if (admit_python_record()) {
                    handle.finish();
                }
            },
            "Send total/total if that is not the state sent last, and end the bar.")
        .def_property_readonly("current", &ProgressHandle::current)
        .def_property_readonly("total", &ProgressHandle::total)
        .def_property_readonly("finished", &ProgressHandle::finished);

    m.def(
        "progress",
        [](const py::str& label, uint64_t total) {
            return lumen::progress(utf8(label, "label"), total);
        },
        py::arg("label"), py::arg("total"),
        "A new progress bar of `total` units. Creating it sends nothing.");

    m.def("now_ns", &now_ns, "The library's clock (the one records are stamped with), in ns.");
}

}  // namespace

void register_producers(py::module_& m) {
    register_logging(m);
    register_metric_and_progress(m);
}

}  // namespace lumen::python
