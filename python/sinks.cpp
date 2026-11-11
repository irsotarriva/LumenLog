// The library's own sinks. This file is the only one that knows their
// classes and configurations, so that a change to lumen/sink.hpp is made here
// alone.

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "immortal.hpp"
#include "lumen/sink.hpp"
#include "registration.hpp"

namespace py = pybind11;

namespace lumen::python {
namespace {

// The configurations hold std::string_view, which must not point into a
// Python string (it can be freed as soon as the assignment returns). So a
// string assigned from Python is copied into this pool, which is never freed,
// and the view a configuration (and the sink built from it) keeps stays
// valid. Equal strings share one copy, so the pool grows only with the number
// of distinct strings assigned. Not needed for configurations that own their
// strings.
struct RetainedStrings {
    std::mutex mutex;
    std::set<std::string, std::less<>> strings;  // guarded by mutex
};

[[nodiscard]] std::string_view retained(std::string_view text) {
    static detail::Immortal<RetainedStrings> pool;
    RetainedStrings& p = pool.get();
    const std::scoped_lock lock(p.mutex);
    return *p.strings.emplace(text).first;
}

[[nodiscard]] std::vector<std::string_view> retained_all(const std::vector<std::string>& texts) {
    std::vector<std::string_view> out;
    out.reserve(texts.size());
    for (const std::string& t : texts) {
        out.push_back(retained(t));
    }
    return out;
}

[[nodiscard]] std::vector<std::string> copies(const std::vector<std::string_view>& views) {
    return {views.begin(), views.end()};
}

void register_enums(py::module_& m) {
    py::enum_<AnsiColor>(m, "AnsiColor")
        .value("Default", AnsiColor::Default)
        .value("Black", AnsiColor::Black)
        .value("Red", AnsiColor::Red)
        .value("Green", AnsiColor::Green)
        .value("Yellow", AnsiColor::Yellow)
        .value("Blue", AnsiColor::Blue)
        .value("Magenta", AnsiColor::Magenta)
        .value("Cyan", AnsiColor::Cyan)
        .value("White", AnsiColor::White);

    py::enum_<FileFormat>(m, "FileFormat")
        .value("PlainText", FileFormat::PlainText)
        .value("KeyValue", FileFormat::KeyValue)
        .value("Json", FileFormat::Json);
}

void register_configs(py::module_& m) {
    using TerminalConfig = TerminalSink::Config;
    py::class_<TerminalConfig>(m, "TerminalConfig")
        .def(py::init<>())
        .def_property(
            "time_format", [](const TerminalConfig& c) { return std::string(c.time_format); },
            [](TerminalConfig& c, std::string_view v) { c.time_format = retained(v); })
        .def_property(
            "inline_tags", [](const TerminalConfig& c) { return copies(c.inline_tags); },
            [](TerminalConfig& c, const std::vector<std::string>& v) {
                c.inline_tags = retained_all(v);
            })
        .def_property(
            "dashboard_metrics",
            [](const TerminalConfig& c) { return copies(c.dashboard_metrics); },
            [](TerminalConfig& c, const std::vector<std::string>& v) {
                c.dashboard_metrics = retained_all(v);
            })
        .def_readwrite("dashboard_height", &TerminalConfig::dashboard_height)
        .def_readwrite("enable_dashboard", &TerminalConfig::enable_dashboard)
        .def_readwrite("colors", &TerminalConfig::colors);

    using FileConfig = FileSink::Config;
    py::class_<FileConfig>(m, "FileConfig")
        .def(py::init<>())
        .def_property(
            "path", [](const FileConfig& c) { return std::string(c.path); },
            [](FileConfig& c, std::string_view v) { c.path = retained(v); })
        .def_readwrite("rotate_mb", &FileConfig::rotate_mb)
        .def_readwrite("max_files", &FileConfig::max_files)
        .def_readwrite("format", &FileConfig::format);
}

}  // namespace

// The sinks expose no methods to Python: once registered they belong to
// Core, which serialises their callbacks, and a call from Python would race
// with it.
void register_sinks(py::module_& m) {
    register_enums(m);
    register_configs(m);

    py::classh<TerminalSink, Sink>(m, "TerminalSink", "Writes records to the terminal.")
        .def(py::init<TerminalSink::Config>(), py::arg("config"));

    py::classh<FileSink, Sink>(m, "FileSink", "Writes records to a file, with rotation.")
        .def(py::init<FileSink::Config>(), py::arg("config"));

    py::classh<JsonSink, Sink>(m, "JsonSink", "Writes records to a file as JSON lines.")
        .def(py::init<std::string_view>(), py::arg("path"));

    py::classh<NullSink, Sink>(m, "NullSink", "Discards every record.").def(py::init<>());
}

}  // namespace lumen::python
