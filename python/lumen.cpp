#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/functional.h>

#include <memory>
#include <cstring>
#include <string>

#include "lumen/lumen.h"

namespace py = pybind11;
using namespace lumen;

namespace {

struct PySink : Sink, py::trampoline_self_life_support {
    void on_log(const LogRecord& record) override {
        try {
            PYBIND11_OVERRIDE_PURE(void, Sink, on_log, record);
        } catch (const py::error_already_set& e) {
            std::fprintf(stderr, "lumen: Python exception in Sink::on_log: %s\n", e.what());
        }
    }
    void on_metric(const MetricRecord& record) override {
        try {
            PYBIND11_OVERRIDE_PURE(void, Sink, on_metric, record);
        } catch (const py::error_already_set& e) {
            std::fprintf(stderr, "lumen: Python exception in Sink::on_metric: %s\n", e.what());
        }
    }
    void on_progress(const ProgressRecord& record) override {
        try {
            PYBIND11_OVERRIDE_PURE(void, Sink, on_progress, record);
        } catch (const py::error_already_set& e) {
            std::fprintf(stderr, "lumen: Python exception in Sink::on_progress: %s\n", e.what());
        }
    }
    void flush() override {
        try {
            PYBIND11_OVERRIDE_PURE(void, Sink, flush,);
        } catch (const py::error_already_set& e) {
            std::fprintf(stderr, "lumen: Python exception in Sink::flush: %s\n", e.what());
        }
    }
};

static std::string_view py_arena_copy(std::string_view s) {
    char* buf = this_thread_arena.allocate(s.size() + 1);
    std::memcpy(buf, s.data(), s.size());
    buf[s.size()] = '\0';
    return std::string_view(buf, s.size());
}

void py_log_info(std::string msg) {
    RecordBuilder(core().log_buffer(), LogLevel::INFO, py_arena_copy(msg),
                  std::source_location::current());
}

void py_log_warn(std::string msg) {
    RecordBuilder(core().log_buffer(), LogLevel::WARN, py_arena_copy(msg),
                  std::source_location::current());
}

void py_log_error(std::string msg) {
    RecordBuilder(core().log_buffer(), LogLevel::ERROR, py_arena_copy(msg),
                  std::source_location::current());
}

void py_log_debug(std::string msg) {
    RecordBuilder(core().log_buffer(), LogLevel::DEBUG, py_arena_copy(msg),
                  std::source_location::current());
}

void py_log_fatal(std::string msg) {
    {
        RecordBuilder(core().log_buffer(), LogLevel::FATAL, py_arena_copy(msg),
                      std::source_location::current());
    }
    core().flush();
    std::terminate();
}

void py_metric(std::string name, double value) {
    MetricBuilder(core().metric_buffer(), py_arena_copy(name), value);
}

struct PyProgressHandle {
    explicit PyProgressHandle(std::string label, uint64_t total)
        : __handle(core().progress_buffer(), label, total) {}

    void update(uint64_t current) { __handle.update(current); }
    void tick() { __handle.tick(); }
    void finish() { __handle.finish(); }

private:
    ProgressHandle __handle;
};

class PyProgressHandleWrapper {
public:
    PyProgressHandleWrapper(std::string label, uint64_t total)
        : __handle(std::make_unique<ProgressHandle>(
              core().progress_buffer(), py_arena_copy(label), total)) {}

    PyProgressHandleWrapper(PyProgressHandleWrapper&&) noexcept = default;
    PyProgressHandleWrapper& operator=(PyProgressHandleWrapper&&) noexcept = default;
    PyProgressHandleWrapper(const PyProgressHandleWrapper&) = delete;
    PyProgressHandleWrapper& operator=(const PyProgressHandleWrapper&) = delete;

    void update(uint64_t current) { __handle->update(current); }
    void tick() { __handle->tick(); }
    void finish() {
        if (__handle) {
            __handle->finish();
            __handle.reset();
        }
    }

private:
    std::unique_ptr<ProgressHandle> __handle;
};

class PySinkWrapper : public Sink {
public:
    PySinkWrapper(py::object on_log_fn, py::object on_metric_fn,
                  py::object on_progress_fn, py::object flush_fn)
        : __on_log(std::move(on_log_fn))
        , __on_metric(std::move(on_metric_fn))
        , __on_progress(std::move(on_progress_fn))
        , __flush(std::move(flush_fn)) {}

    void on_log(const LogRecord& record) override {
        try {
            py::gil_scoped_acquire gil;
            if (!__on_log.is_none()) __on_log(record);
        } catch (const py::error_already_set& e) {
            std::fprintf(stderr, "lumen: Python exception in CallbackSink::on_log: %s\n", e.what());
        }
    }
    void on_metric(const MetricRecord& record) override {
        try {
            py::gil_scoped_acquire gil;
            if (!__on_metric.is_none()) __on_metric(record);
        } catch (const py::error_already_set& e) {
            std::fprintf(stderr, "lumen: Python exception in CallbackSink::on_metric: %s\n", e.what());
        }
    }
    void on_progress(const ProgressRecord& record) override {
        try {
            py::gil_scoped_acquire gil;
            if (!__on_progress.is_none()) __on_progress(record);
        } catch (const py::error_already_set& e) {
            std::fprintf(stderr, "lumen: Python exception in CallbackSink::on_progress: %s\n", e.what());
        }
    }
    void flush() override {
        try {
            py::gil_scoped_acquire gil;
            if (!__flush.is_none()) __flush();
        } catch (const py::error_already_set& e) {
            std::fprintf(stderr, "lumen: Python exception in CallbackSink::flush: %s\n", e.what());
        }
    }

private:
    py::object __on_log;
    py::object __on_metric;
    py::object __on_progress;
    py::object __flush;
};

}  // namespace

PYBIND11_MODULE(lumen_bindings, m) {
    m.doc() = "Lumen — zero-instrumentation structured telemetry for C++20";

    py::class_<TagSet<16>::Entry>(m, "TagEntry")
        .def_readonly("key", &TagSet<16>::Entry::key)
        .def_readonly("value", &TagSet<16>::Entry::value);

    py::class_<TagSet<16>>(m, "TagSet")
        .def(py::init<>())
        .def("add", &TagSet<16>::add,
             py::arg("key"), py::arg("value"))
        .def("find", &TagSet<16>::find, py::arg("key"))
        .def("count", &TagSet<16>::count)
        .def("__iter__", [](const TagSet<16>& ts) {
            return py::make_iterator(ts.begin(), ts.end());
        });

    py::enum_<LogLevel>(m, "LogLevel")
        .value("TRACE", LogLevel::TRACE)
        .value("DEBUG", LogLevel::DEBUG)
        .value("INFO",  LogLevel::INFO)
        .value("WARN",  LogLevel::WARN)
        .value("ERROR", LogLevel::ERROR)
        .value("FATAL", LogLevel::FATAL);

    py::class_<SourceLocation>(m, "SourceLocation")
        .def_readonly("file", &SourceLocation::file)
        .def_readonly("function", &SourceLocation::function)
        .def_readonly("class_name", &SourceLocation::class_name)
        .def_readonly("line", &SourceLocation::line);

    py::class_<LogRecord>(m, "LogRecord")
        .def(py::init<>())
        .def_readwrite("level", &LogRecord::level)
        .def_readwrite("message", &LogRecord::message)
        .def_readwrite("tags", &LogRecord::tags)
        .def_readwrite("timestamp_ns", &LogRecord::timestamp_ns)
        .def_readwrite("thread_id", &LogRecord::thread_id)
        .def_readwrite("source", &LogRecord::source);

    py::class_<MetricRecord>(m, "MetricRecord")
        .def(py::init<>())
        .def_readwrite("name", &MetricRecord::name)
        .def_readwrite("value", &MetricRecord::value)
        .def_readwrite("tags", &MetricRecord::tags)
        .def_readwrite("timestamp_ns", &MetricRecord::timestamp_ns);

    py::class_<ProgressRecord>(m, "ProgressRecord")
        .def(py::init<>())
        .def_readwrite("id", &ProgressRecord::id)
        .def_readwrite("label", &ProgressRecord::label)
        .def_readwrite("current", &ProgressRecord::current)
        .def_readwrite("total", &ProgressRecord::total)
        .def_readwrite("tags", &ProgressRecord::tags)
        .def_readwrite("timestamp_ns", &ProgressRecord::timestamp_ns);

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

    py::classh<Sink, PySink>(m, "Sink")
        .def(py::init<>())
        .def("on_log", &Sink::on_log)
        .def("on_metric", &Sink::on_metric)
        .def("on_progress", &Sink::on_progress)
        .def("flush", &Sink::flush);

    py::class_<TerminalSink::Config>(m, "TerminalConfig")
        .def(py::init<>())
        .def_readwrite("time_format", &TerminalSink::Config::time_format)
        .def_readwrite("inline_tags", &TerminalSink::Config::inline_tags)
        .def_readwrite("dashboard_metrics", &TerminalSink::Config::dashboard_metrics)
        .def_readwrite("dashboard_height", &TerminalSink::Config::dashboard_height)
        .def_readwrite("enable_dashboard", &TerminalSink::Config::enable_dashboard)
        .def_readwrite("colors", &TerminalSink::Config::colors);

    py::class_<FileSink::Config>(m, "FileConfig")
        .def(py::init<>())
        .def_readwrite("path", &FileSink::Config::path)
        .def_readwrite("rotate_mb", &FileSink::Config::rotate_mb)
        .def_readwrite("max_files", &FileSink::Config::max_files)
        .def_readwrite("format", &FileSink::Config::format);

    py::classh<TerminalSink, Sink>(m, "TerminalSink")
        .def(py::init<TerminalSink::Config>(), py::arg("config"))
        .def("on_log", &TerminalSink::on_log)
        .def("on_metric", &TerminalSink::on_metric)
        .def("on_progress", &TerminalSink::on_progress)
        .def("flush", &TerminalSink::flush);

    py::classh<FileSink, Sink>(m, "FileSink")
        .def(py::init<FileSink::Config>(), py::arg("config"))
        .def("on_log", &FileSink::on_log)
        .def("on_metric", &FileSink::on_metric)
        .def("on_progress", &FileSink::on_progress)
        .def("flush", &FileSink::flush);

    py::classh<JsonSink, Sink>(m, "JsonSink")
        .def(py::init<std::string_view>(), py::arg("path"))
        .def("on_log", &JsonSink::on_log)
        .def("on_metric", &JsonSink::on_metric)
        .def("on_progress", &JsonSink::on_progress)
        .def("flush", &JsonSink::flush);

    py::classh<NullSink, Sink>(m, "NullSink")
        .def(py::init<>());

    py::classh<PySinkWrapper, Sink>(m, "CallbackSink")
        .def(py::init<py::object, py::object, py::object, py::object>(),
             py::arg("on_log") = py::none(),
             py::arg("on_metric") = py::none(),
             py::arg("on_progress") = py::none(),
             py::arg("flush") = py::none());

    py::class_<Predicate>(m, "Predicate")
        .def(py::init<>())
        .def("evaluate", &Predicate::evaluate,
             py::arg("tags"), py::arg("level"))
        .def("__and__", [](const Predicate& a, const Predicate& b) {
            return a && b;
        })
        .def("__or__", [](const Predicate& a, const Predicate& b) {
            return a || b;
        })
        .def("__invert__", [](const Predicate& p) {
            return !p;
        });

    m.def("always", &always);
    m.def("never", &never);
    m.def("level_at_least", &level_at_least, py::arg("min_level"));
    m.def("level_equals", &level_equals, py::arg("level"));
    m.def("tag_equals", &tag_equals, py::arg("key"), py::arg("value"));
    m.def("tag_exists", &tag_exists, py::arg("key"));

    py::class_<Core, std::unique_ptr<Core, py::nodelete>>(m, "Core")
        .def("add_sink",
             [](Core& c, std::unique_ptr<Sink> sink, Predicate pred) -> SinkId {
                 return c.add_sink(std::move(sink), std::move(pred));
             },
             py::arg("sink"), py::arg("predicate"))
        .def("remove_sink",
             [](Core& c, SinkId id) -> std::unique_ptr<Sink> {
                 auto result = c.remove_sink(id);
                 if (result.has_value()) {
                     return std::move(*result);
                 }
                 return nullptr;
             },
             py::arg("id"))
        .def("flush", &Core::flush)
        .def("set_process_tag", &Core::set_process_tag,
             py::arg("key"), py::arg("value"))
        .def("set_thread_tag", &Core::set_thread_tag,
             py::arg("key"), py::arg("value"))
        .def("emit_log",
             [](Core& c, LogRecord rec) { c.emit(std::move(rec)); })
        .def("emit_metric",
             [](Core& c, MetricRecord rec) { c.emit(std::move(rec)); })
        .def("emit_progress",
             [](Core& c, ProgressRecord rec) { c.emit(std::move(rec)); });

    m.def("core", &core, py::return_value_policy::reference);

    py::class_<PyProgressHandleWrapper>(m, "Progress")
        .def(py::init<std::string, uint64_t>(),
             py::arg("label"), py::arg("total"))
        .def("update", &PyProgressHandleWrapper::update, py::arg("current"))
        .def("tick", &PyProgressHandleWrapper::tick)
        .def("finish", &PyProgressHandleWrapper::finish);

    m.def("info", &py_log_info, py::arg("msg"),
          "Emit an INFO-level log record");
    m.def("warn", &py_log_warn, py::arg("msg"),
          "Emit a WARN-level log record");
    m.def("error", &py_log_error, py::arg("msg"),
          "Emit an ERROR-level log record");
    m.def("debug", &py_log_debug, py::arg("msg"),
          "Emit a DEBUG-level log record");
    m.def("fatal", &py_log_fatal, py::arg("msg"),
          "Emit a FATAL-level log record and terminate");
    m.def("metric", &py_metric, py::arg("name"), py::arg("value"),
          "Emit a metric record");
    m.def("progress",
          [](std::string label, uint64_t total) {
              return PyProgressHandleWrapper(std::move(label), total);
          },
          py::arg("label"), py::arg("total"),
          "Create a progress handle");
    m.def("set_process_tag", &set_process_tag,
          py::arg("key"), py::arg("value"));
    m.def("set_thread_tag", &set_thread_tag,
          py::arg("key"), py::arg("value"));
    m.def("flush", []() { core().flush(); });
    m.def("now_ns", &now_ns, "Current time in nanoseconds since epoch");
}
