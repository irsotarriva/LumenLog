#include <pybind11/pybind11.h>

#include <iostream>
#include <memory>
#include <string_view>
#include <utility>

#include "errors.hpp"
#include "interpreter_exit.hpp"
#include "lumen/core.hpp"
#include "lumen/predicate.hpp"
#include "lumen/sink.hpp"
#include "python_sinks.hpp"
#include "registration.hpp"

namespace py = pybind11;

namespace lumen::python {
namespace {

// A Python callable as a DiagnosticHandler. Core copies its handler on the
// delivering thread without the GIL, so the callable is held through a
// shared_ptr: copies change no Python reference count.
class PythonDiagnostics {
public:
    explicit PythonDiagnostics(py::function handler)
        : held_(std::make_shared<Held>(std::move(handler))) {}

    // An exception raised by the handler has no caller to go to; it is
    // passed to sys.unraisablehook. Once Python can no longer run code the
    // line goes to std::cerr instead.
    void operator()(std::string_view line) const {
        const PythonCall admission(PythonCallKind::Other);
        if (!admission.admitted()) {
            std::cerr << line << '\n' << std::flush;
            return;
        }
        const py::gil_scoped_acquire gil;
        try {
            held_->handler(py::str(line.data(), line.size()));
        } catch (py::error_already_set& e) {
            e.discard_as_unraisable("lumen diagnostic handler");
        }
    }

private:
    struct Held {
        explicit Held(py::function f) : handler(std::move(f)) {}
        // Released with the GIL; left alone once Python cannot run code.
        ~Held() {
            const PythonCall admission(PythonCallKind::Other);
            if (!admission.admitted()) {
                static_cast<void>(handler.release());
                return;
            }
            try {
                const py::gil_scoped_acquire gil;
                handler = py::function();
            } catch (...) {
                // pybind11 throws when it cannot take the GIL; the callable
                // is then left alone, as after exit.
                static_cast<void>(handler.release());
            }
        }
        Held(const Held&) = delete;
        Held& operator=(const Held&) = delete;
        Held(Held&&) = delete;
        Held& operator=(Held&&) = delete;

        py::function handler;
    };
    std::shared_ptr<Held> held_;
};

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
    if (handler.is_none()) {
        set_diagnostic_handler(c, DiagnosticHandler{}, false);
        return;
    }
    if (PyCallable_Check(handler.ptr()) == 0) {
        throw py::type_error("the diagnostic handler must be callable or None");
    }
    set_diagnostic_handler(c, PythonDiagnostics(py::reinterpret_borrow<py::function>(handler)),
                           true);
}

void register_statistics(py::module_& m) {
    py::class_<QueueStatistics>(m, "QueueStatistics",
                                "Counters of one record queue since the Core was created.")
        .def_readonly("accepted", &QueueStatistics::accepted, "Records queued for delivery.")
        .def_readonly("dropped_queue_full", &QueueStatistics::dropped_queue_full,
                      "Records refused because the queue was full.")
        .def_readonly("dropped_after_shutdown", &QueueStatistics::dropped_after_shutdown,
                      "Records submitted after the Core shut down.");

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
        .def("statistics", &Core::statistics, "Counters since the Core was created.")
        .def("set_diagnostic_handler", &set_handler, py::arg("handler"),
             "Replace the handler that receives the library's diagnostic lines (the first "
             "record lost to a full queue, the first exception of each sink, ...). `handler` "
             "is called with one str, on the library's dispatch thread; None discards the "
             "lines. An exception it raises goes to sys.unraisablehook.")
        .def(
            "reset_diagnostic_handler",
            [](Core& c) { set_diagnostic_handler(c, stderr_diagnostics(), false); },
            "Put back the default diagnostic handler, which writes each line to stderr.");

    m.def("core", &core, py::return_value_policy::reference, "The process-wide Core.");
}

}  // namespace lumen::python
