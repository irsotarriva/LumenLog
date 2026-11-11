#ifndef LUMEN_PYTHON_INTERPRETER_EXIT_HPP
#define LUMEN_PYTHON_INTERPRETER_EXIT_HPP

#include <memory>

#include "lumen/core.hpp"
#include "lumen/predicate.hpp"
#include "lumen/sink.hpp"

// The global lumen::core() outlives the Python interpreter: it shuts down at
// C++ static destruction, after Py_Finalize. Anything of Python's that it
// still holds then (a sink written in Python, a Python diagnostic handler)
// would be called or released without an interpreter, which aborts the
// process. So the module registers an atexit function (register_interpreter_exit)
// that runs while the interpreter is still whole, and:
//
//   1. refuses further Python sinks (add_python_sink raises RuntimeError);
//   2. delivers every record queued so far (Core::flush), so Python sinks
//      receive everything logged before exit;
//   3. removes every Python sink from core() and destroys it, with the GIL;
//   4. puts back the default diagnostic handler if Python had replaced it;
//   5. closes the gate PythonCall checks, and waits until no thread is still
//      inside a call into Python that passed it.
//
// After that no C++ thread calls into Python again; sinks written in C++ keep
// working until static destruction. This also covers an unhandled exception
// and sys.exit(), which run atexit functions too. os._exit() and a fatal
// signal skip both atexit and C++ static destruction, so neither runs.

namespace lumen::python {

// Admission of one call into Python from a C++ thread that may not hold the
// GIL. Create one before acquiring the GIL; if admitted() is false, Python can
// no longer run code (the exit hook ran, or the interpreter is not
// initialised or is finalising) and the call must be skipped. The exit hook
// waits for every admitted PythonCall to be destroyed.
class PythonCall {
public:
    PythonCall() noexcept;
    ~PythonCall();
    PythonCall(const PythonCall&) = delete;
    PythonCall& operator=(const PythonCall&) = delete;
    PythonCall(PythonCall&&) = delete;
    PythonCall& operator=(PythonCall&&) = delete;

    [[nodiscard]] bool admitted() const noexcept { return admitted_; }

private:
    bool admitted_;
};

// Registers a sink whose callbacks run Python code with `core`, so that the
// exit hook detaches it. Called with the GIL held; releases it around the
// registration. Raises RuntimeError once the exit hook has run, and the
// library's error for a null sink.
[[nodiscard]] SinkId add_python_sink(Core& core, std::unique_ptr<Sink> sink, Predicate predicate);

// Forgets a sink that was removed from core() (no-op for other ids).
void forget_python_sink(SinkId id) noexcept;

// Core::set_diagnostic_handler on `core`, recording whether `handler` calls
// Python, so that the exit hook knows to put the default one back. Called
// with the GIL held, which it keeps (destroying the previous handler may need
// it). Raises RuntimeError for a handler that calls Python once the exit hook
// has run.
void set_diagnostic_handler(Core& core, DiagnosticHandler handler, bool calls_python);

}  // namespace lumen::python

#endif
