#include <pybind11/pybind11.h>

#include <chrono>
#include <expected>
#include <memory>
#include <string_view>
#include <system_error>
#include <utility>

#include "errors.hpp"
#include "interpreter_exit.hpp"
#include "lumen/core.hpp"
#include "lumen/predicate.hpp"
#include "lumen/sink.hpp"
#include "python_diagnostics.hpp"
#include "python_sinks.hpp"
#include "registration.hpp"

namespace py = pybind11;

namespace lumen::python {
namespace {

[[nodiscard]] SinkId add_sink(Core& c, std::unique_ptr<Sink> sink, Predicate predicate) {
    if (dynamic_cast<PythonSink*>(sink.get()) != nullptr) {
        return add_python_sink(c, std::move(sink), std::move(predicate));
    }
    std::expected<SinkId, std::error_code> added;
    {
        // A sink written in C++ needs no GIL, not even when destroyed.
        const py::gil_scoped_release release;
        added = c.add_sink(std::move(sink), std::move(predicate));
    }
    return value_or_raise(std::move(added));
}

[[nodiscard]] std::unique_ptr<Sink> remove_sink(Core& c, SinkId id) {
    std::expected<std::unique_ptr<Sink>, std::error_code> removed;
    {
        // Waits for a running callback of this sink, which may need the GIL.
        const py::gil_scoped_release release;
        removed = c.remove_sink(id);
    }
    // From here on the GIL is held, so a Python sink can be handed back.
    std::unique_ptr<Sink> sink = value_or_raise(std::move(removed));
    forget_python_sink(id);
    return sink;
}

void set_handler(Core& c, const py::object& handler) {
    bool calls_python = false;
    const DiagnosticHandler converted = diagnostics_from_python(handler, calls_python);
    set_diagnostic_handler(c, converted, calls_python);
}

// The timeout in seconds, as Core::flush_for takes it: a negative or NaN one
// is refused, and one beyond the range of milliseconds means no limit.
void flush_for(Core& c, double timeout_s) {
    if (!(timeout_s >= 0.0)) {
        throw py::value_error("the timeout must be a number of seconds, at least 0");
    }
    const std::chrono::duration<double> seconds(timeout_s);
    const std::chrono::milliseconds timeout =
        seconds < std::chrono::duration<double>(std::chrono::milliseconds::max())
            ? std::chrono::duration_cast<std::chrono::milliseconds>(seconds)
            : std::chrono::milliseconds::max();
    std::expected<void, std::error_code> flushed;
    {
        // The dispatch thread runs the Python sinks meanwhile.
        const py::gil_scoped_release release;
        flushed = c.flush_for(timeout);
    }
    value_or_raise(flushed);
}

void set_shutdown_timeout_s(Core& c, double timeout_s) {
    if (!(timeout_s >= 0.0)) {
        throw py::value_error("the timeout must be a number of seconds, at least 0");
    }
    const std::chrono::duration<double> seconds(timeout_s);
    c.set_shutdown_timeout(seconds < std::chrono::duration<double>(std::chrono::milliseconds::max())
                               ? std::chrono::duration_cast<std::chrono::milliseconds>(seconds)
                               : std::chrono::milliseconds::max());
}

void register_statistics(py::module_& m) {
    py::class_<QueueStatistics>(m, "QueueStatistics",
                                "Counters of one record queue since the Core was created.")
        .def_readonly("accepted", &QueueStatistics::accepted, "Records queued for delivery.")
        .def_readonly("dropped_queue_full", &QueueStatistics::dropped_queue_full,
                      "Records refused because the queue was full.")
        .def_readonly("dropped_after_shutdown", &QueueStatistics::dropped_after_shutdown,
                      "Records submitted after the Core shut down.")
        .def_readonly("queued", &QueueStatistics::queued,
                      "Records accepted and not yet handed to the sinks.");

    py::class_<CoreStatistics>(m, "CoreStatistics",
                               "A snapshot of Core.statistics(): one QueueStatistics per record "
                               "type and the number of tags left out of full records.")
        .def_readonly("logs", &CoreStatistics::logs)
        .def_readonly("metrics", &CoreStatistics::metrics)
        .def_readonly("progress", &CoreStatistics::progress)
        .def_readonly("tags_dropped", &CoreStatistics::tags_dropped);
}

}  // namespace

void register_core(py::module_& m) {
    register_statistics(m);

    py::class_<Core, std::unique_ptr<Core, py::nodelete>>(
        m, "Core",
        "The broker between producers and sinks. The module uses the process-wide one, "
        "returned by core(); it cannot be created from Python.")
        .def("add_sink", &add_sink, py::arg("sink"), py::arg("predicate") = always(),
             "Register a sink and return its id. It receives every record dispatched from now "
             "on for which `predicate` is true. The sink belongs to the library until "
             "remove_sink returns it. Raises RuntimeError for a Python sink once the "
             "interpreter is exiting.")
        .def("remove_sink", &remove_sink, py::arg("id"),
             "Unregister a sink and return it. When this returns the sink is not in a callback "
             "and is never called again; if one of its callbacks is running on another thread, "
             "this waits for it. Raises InvalidSinkIdError for an unknown id and SinkBusyError "
             "when called from the sink's own callback.")
        .def(
            "sink_exception_count",
            [](const Core& c, SinkId id) { return value_or_raise(c.sink_exception_count(id)); },
            py::arg("id"),
            "How many exceptions this sink's callbacks have raised. Raises InvalidSinkIdError "
            "for an unknown id.")
        .def("flush", &Core::flush, py::call_guard<py::gil_scoped_release>(),
             "Deliver every record submitted before the call, then flush every sink, on the "
             "calling thread; returns when that is done. Does nothing when called from a sink "
             "callback.")
        .def("flush_for", &flush_for, py::arg("timeout"),
             "As flush, but the library's dispatch thread does the work and this waits at most "
             "`timeout` seconds. Raises FlushTimedOutError when the time runs out (the flush "
             "is still made later), SinkBusyError from inside a sink callback or when the "
             "wait would never end, LumenError after shutdown, and ValueError for a negative "
             "timeout.")
        .def("statistics", &Core::statistics, "Counters since the Core was created.")
        .def("set_diagnostic_handler", &set_handler, py::arg("handler"),
             "Replace the handler that receives the library's diagnostic lines (the first "
             "record lost to a full queue, the first exception of each sink, ...). `handler` "
             "is called with one str, on the library's dispatch thread; None discards the "
             "lines. An exception it raises goes to sys.unraisablehook.")
        .def(
            "reset_diagnostic_handler",
            [](Core& c) { set_diagnostic_handler(c, stderr_diagnostics(), false); },
            "Put back the default diagnostic handler, which writes each line to stderr.")
        .def("set_shutdown_timeout", &set_shutdown_timeout_s, py::arg("timeout"),
             "How long, in seconds, the library's shutdown (after the interpreter has exited) "
             "and a fatal record wait for a sink callback that makes no progress before giving "
             "up on it; 5 to start with. ValueError for a negative timeout.");

    m.def("core", &core, py::return_value_policy::reference, "The process-wide Core.");
}

}  // namespace lumen::python
