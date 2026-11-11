// The lumen_bindings Python module. Needs pybind11 3.0 or later (py::classh,
// py::native_enum) and Python 3.11 or later (co_qualname).

#include <pybind11/pybind11.h>

#include "registration.hpp"

PYBIND11_MODULE(lumen_bindings, m) {
    m.doc() =
        "Lumen structured telemetry from Python, on the same broker as the C++ API.\n\n"
        "Producers: trace, debug, info, warn, error, log, fatal, metric, progress. Each takes "
        "an optional tags= dict; log records carry the calling Python file, line and function "
        "as their source (set_source_capture turns this off). The C++ compile-time level "
        "threshold does not apply to records logged from Python.\n"
        "Context: set/remove/clear_thread_tag(s), set/remove/clear_process_tag(s), and the "
        "scope() context manager.\n"
        "Sinks: subclass Sink or use CallbackSink, or use the library's sinks; register them "
        "with core().add_sink(sink, predicate).\n\n"
        "Errors are raised as LumenError subclasses: InvalidSinkIdError (also a LookupError), "
        "SinkBusyError, TagTableFullError and InvalidPredicateError (also a ValueError); an "
        "operating-system error as OSError. Wrong argument types raise TypeError.\n\n"
        "At interpreter exit the module delivers what is queued to Python sinks, then detaches "
        "them; sinks written in C++ keep receiving records until the process ends.";

    lumen::python::register_errors(m);
    lumen::python::register_records(m);
    lumen::python::register_predicates(m);
    lumen::python::register_python_sinks(m);
    lumen::python::register_sinks(m);
    lumen::python::register_core(m);
    lumen::python::register_producers(m);
    lumen::python::register_context(m);
    lumen::python::register_interpreter_exit(m);
}
