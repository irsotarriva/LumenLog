#include "python_sinks.hpp"

#include <pybind11/pybind11.h>

#include <string>
#include <utility>

#include "interpreter_exit.hpp"
#include "lumen/record.hpp"
#include "lumen/sink.hpp"
#include "registration.hpp"

namespace py = pybind11;

namespace lumen::python {
namespace {

// Records are passed to Python as copies. A copy shares the record's string
// storage, so a sink may keep it after the callback returns.
template <typename R>
[[nodiscard]] py::object record_copy(const R& record) {
    return py::cast(record, py::return_value_policy::copy);
}

// Lets a Python class derive from lumen.Sink. Each callback calls the Python
// method of that name if the class defines one; the defaults do nothing.
class SinkTrampoline : public PythonSink, public py::trampoline_self_life_support {
public:
    void on_log(const LogRecord& record) override { call_override_("on_log", record); }
    void on_metric(const MetricRecord& record) override { call_override_("on_metric", record); }
    void on_progress(const ProgressRecord& record) override {
        call_override_("on_progress", record);
    }
    void flush() override {
        _call_python([this] {
            if (const py::function f = py::get_override(static_cast<const Sink*>(this), "flush")) {
                f();
            }
        });
    }

private:
    template <typename R>
    void call_override_(const char* name, const R& record) {
        _call_python([this, name, &record] {
            if (const py::function f = py::get_override(static_cast<const Sink*>(this), name)) {
                f(record_copy(record));
            }
        });
    }
};

// A sink made of up to four Python callables, any of which may be None.
class CallbackSink : public PythonSink {
public:
    CallbackSink(py::object on_log, py::object on_metric, py::object on_progress, py::object flush)
        : on_log_(checked_(std::move(on_log), "on_log"))
        , on_metric_(checked_(std::move(on_metric), "on_metric"))
        , on_progress_(checked_(std::move(on_progress), "on_progress"))
        , flush_(checked_(std::move(flush), "flush")) {}

    // Core may destroy a sink on any thread, so the callables are released
    // with the GIL; once Python can no longer run code they are left alone
    // (released without a reference count change), since touching them would
    // abort.
    ~CallbackSink() override {
        const PythonCall admission;
        if (!admission.admitted()) {
            static_cast<void>(on_log_.release());
            static_cast<void>(on_metric_.release());
            static_cast<void>(on_progress_.release());
            static_cast<void>(flush_.release());
            return;
        }
        const py::gil_scoped_acquire gil;
        on_log_ = py::object();
        on_metric_ = py::object();
        on_progress_ = py::object();
        flush_ = py::object();
    }

    CallbackSink(const CallbackSink&) = delete;
    CallbackSink& operator=(const CallbackSink&) = delete;
    CallbackSink(CallbackSink&&) = delete;
    CallbackSink& operator=(CallbackSink&&) = delete;

    void on_log(const LogRecord& record) override { call_(on_log_, record); }
    void on_metric(const MetricRecord& record) override { call_(on_metric_, record); }
    void on_progress(const ProgressRecord& record) override { call_(on_progress_, record); }
    void flush() override {
        if (flush_) {
            _call_python([this] { flush_(); });
        }
    }

private:
    // None becomes an empty object, so a callback that is not set costs no GIL.
    [[nodiscard]] static py::object checked_(py::object callable, const char* name) {
        if (callable.is_none()) {
            return {};
        }
        if (PyCallable_Check(callable.ptr()) == 0) {
            throw py::type_error(std::string(name) + " must be callable or None");
        }
        return callable;
    }

    template <typename R>
    void call_(const py::object& callable, const R& record) {
        if (callable) {
            _call_python([&callable, &record] { callable(record_copy(record)); });
        }
    }

    py::object on_log_;
    py::object on_metric_;
    py::object on_progress_;
    py::object flush_;
};

}  // namespace

std::string describe(const py::error_already_set& e) {
    std::string text = py::str(e.type().attr("__qualname__"));
    const std::string message = py::str(e.value());
    if (!message.empty()) {
        text += ": " + message;
    }
    return text;
}

void register_python_sinks(py::module_& m) {
    py::classh<Sink, SinkTrampoline>(
        m, "Sink",
        "Base class for sinks written in Python. Define any of on_log(record), "
        "on_metric(record), on_progress(record) and flush(); the ones not defined do nothing. "
        "A subclass that defines __init__ must call super().__init__().\n\n"
        "The methods are called on the library's dispatch thread (or on the thread that calls "
        "Core.flush), one at a time, with the GIL held. Records are copies the sink may keep. "
        "An exception raised by a method is counted (Core.sink_exception_count) and the first "
        "one is reported through the diagnostic handler; delivery to other sinks continues.\n\n"
        "Core.add_sink takes the sink over: its C++ part belongs to the library until "
        "Core.remove_sink gives it back. Python attributes of the object stay readable.")
        .def(py::init<>());

    py::classh<CallbackSink, Sink>(
        m, "CallbackSink",
        "A sink made of callables: on_log(record), on_metric(record), on_progress(record) and "
        "flush(), each optional (None). Called like the methods of a Sink subclass.")
        .def(py::init<py::object, py::object, py::object, py::object>(),
             py::arg("on_log") = py::none(), py::arg("on_metric") = py::none(),
             py::arg("on_progress") = py::none(), py::arg("flush") = py::none());
}

}  // namespace lumen::python
