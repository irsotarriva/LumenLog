// The library's own sinks. This file is the only one that knows their
// classes and configurations, so that a change to the sink headers is made
// here alone.
//
// The configurations own their strings and paths, so binding their fields
// directly copies Python's values. The configurations' `output` stream and
// `diagnostics` handler are not bound: terminal output goes to stderr, and
// a sink's diagnostics go to the default handler (stderr).

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl/filesystem.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

#include "errors.hpp"
#include "lumen/error.hpp"
#include "lumen/file_sink.hpp"
#include "lumen/sink.hpp"
#include "lumen/terminal_sink.hpp"
#include "lumen/terminal_style.hpp"
#include "lumen/writer_statistics.hpp"
#include "registration.hpp"

namespace py = pybind11;

namespace lumen::python {
namespace {

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

    py::enum_<ColorMode>(m, "ColorMode", "Whether a TerminalSink writes ANSI colour escapes.")
        .value("Auto", ColorMode::Auto)
        .value("Always", ColorMode::Always)
        .value("Never", ColorMode::Never);

    py::enum_<TimeZone>(m, "TimeZone", "The clock a TerminalSink shows times in.")
        .value("Local", TimeZone::Local)
        .value("Utc", TimeZone::Utc);

    py::enum_<FileFormat>(m, "FileFormat", "The line format of a FileSink.")
        .value("Text", FileFormat::Text)
        .value("KeyValue", FileFormat::KeyValue)
        .value("Json", FileFormat::Json);
}

void register_configs(py::module_& m) {
    using TerminalConfig = TerminalSink::Config;
    py::class_<TerminalConfig>(m, "TerminalConfig",
                               "Settings of a TerminalSink (lumen/terminal_sink.hpp). Lines go "
                               "to stderr.")
        .def(py::init<>())
        .def_readwrite("color", &TerminalConfig::color)
        .def_readwrite("colors", &TerminalConfig::colors)
        .def_readwrite("time_format", &TerminalConfig::time_format)
        .def_readwrite("time_zone", &TerminalConfig::time_zone)
        .def_readwrite("inline_tags", &TerminalConfig::inline_tags)
        .def_readwrite("show_source", &TerminalConfig::show_source)
        .def_readwrite("max_queued_lines", &TerminalConfig::max_queued_lines);

    using FileConfig = FileSink::Config;
    const FileConfig defaults;
    py::class_<FileConfig>(m, "FileConfig",
                           "Settings of a FileSink (lumen/file_sink.hpp). `path` is a str or "
                           "os.PathLike; \"{date}\" in it becomes the UTC date.")
        .def(py::init([](std::filesystem::path path, FileFormat format, uint64_t max_file_bytes,
                         uint32_t max_files, std::size_t max_queued_lines) {
                 FileConfig c;
                 c.path = std::move(path);
                 c.format = format;
                 c.max_file_bytes = max_file_bytes;
                 c.max_files = max_files;
                 c.max_queued_lines = max_queued_lines;
                 return c;
             }),
             py::arg("path") = defaults.path, py::kw_only(), py::arg("format") = defaults.format,
             py::arg("max_file_bytes") = defaults.max_file_bytes,
             py::arg("max_files") = defaults.max_files,
             py::arg("max_queued_lines") = defaults.max_queued_lines)
        .def_readwrite("path", &FileConfig::path)
        .def_readwrite("format", &FileConfig::format)
        .def_readwrite("max_file_bytes", &FileConfig::max_file_bytes)
        .def_readwrite("max_files", &FileConfig::max_files)
        .def_readwrite("max_queued_lines", &FileConfig::max_queued_lines);

    py::class_<WriterStatistics>(m, "WriterStatistics",
                                 "Counters of a sink's writer thread "
                                 "(lumen/writer_statistics.hpp).")
        .def_readonly("lines_queued", &WriterStatistics::lines_queued)
        .def_readonly("lines_written", &WriterStatistics::lines_written)
        .def_readonly("lines_dropped", &WriterStatistics::lines_dropped)
        .def_readonly("lines_failed", &WriterStatistics::lines_failed);
}

[[nodiscard]] std::unique_ptr<FileSink> create_file_sink(FileSink::Config config) {
    const std::string path = config.path.string();
    auto created = FileSink::create(std::move(config));
    if (!created) {
        raise_error(created.error(), path);
    }
    return std::move(*created);
}

}  // namespace

// Of the sinks' methods only statistics() and path are bound. The callbacks
// and flush() are not: a registered sink belongs to Core, which serialises
// its callbacks. Like any sink, one registered with Core.add_sink cannot be
// used from Python until Core.remove_sink gives it back.
void register_sinks(py::module_& m) {
    register_enums(m);
    register_configs(m);

    py::classh<TerminalSink, Sink>(m, "TerminalSink",
                                   "One human-readable line per record on stderr. Raises "
                                   "InvalidSinkConfigError for an invalid time_format or "
                                   "max_queued_lines == 0.")
        .def(py::init([](TerminalSink::Config config) {
                 return value_or_raise(TerminalSink::create(std::move(config)));
             }),
             py::arg("config") = TerminalSink::Config{})
        .def_property_readonly("colored", &TerminalSink::colored)
        .def("statistics", &TerminalSink::statistics);

    py::classh<FileSink, Sink>(m, "FileSink",
                               "Appends one line per record to a file, with rotation. Raises "
                               "InvalidSinkConfigError for an empty path or a zero count, and "
                               "FileOpenError when the file cannot be opened.")
        .def(py::init(&create_file_sink), py::arg("config"))
        .def_property_readonly("path", &FileSink::path, "The active file, {date} substituted.")
        .def("statistics", &FileSink::statistics);

    py::classh<NullSink, Sink>(m, "NullSink", "Discards every record.").def(py::init<>());
}

}  // namespace lumen::python
