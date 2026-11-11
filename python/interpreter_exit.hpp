#ifndef LUMEN_PYTHON_INTERPRETER_EXIT_HPP
#define LUMEN_PYTHON_INTERPRETER_EXIT_HPP

#include <chrono>
#include <cstdint>
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
//   1. refuses further Python sinks (add_python_sink raises RuntimeError) and
//      further records from Python: from here on the producers (log(),
//      metric(), Progress, ...) drop their record and count it;
//   2. delivers every record queued so far (Core::flush, on a thread of its
//      own), for at most exit_delivery_limit. Then Python sinks stop
//      receiving records: a record that reaches one after that is counted as
//      not delivered. Sinks written in C++ still receive everything;
//   3. waits, for at most exit_callback_limit, until that flush has finished
//      and no thread is inside a call into Python. If only the flush is
//      still going (a sink written in C++ is slow), it goes on on its own
//      thread and the library's shutdown waits for it, but the Python sinks
//      are removed before it reaches the records it has not reached yet;
//      the report says so, without a number;
//   4. removes every Python sink from core() and destroys it, with the GIL;
//   5. puts back the default diagnostic handler if Python had replaced it;
//   6. closes the gate PythonCall checks, so that no C++ thread calls into
//      Python again, and waits (within the same exit_callback_limit) for
//      calls that passed it.
//
// The waits in steps 2, 3 and 6 release the GIL and look for signals: an
// interruption (Ctrl-C) ends the wait it arrives in, as if its limit had
// been reached. So the hook returns within exit_delivery_limit +
// exit_callback_limit (and one look for signals, 50 ms, more).
//
// If a Python callback is still running at the end of step 3 (a sink or
// diagnostic handler that blocks), step 4 and the wait of step 6 are skipped,
// and so is the library's own shutdown at static destruction, which would
// wait for that callback forever: the process ends at the end of interpreter
// finalisation with std::_Exit(EXIT_FAILURE), after flushing the C++
// standard streams. The same happens if a call is still running at the end
// of step 6.
//
// What was not delivered is reported at the end of interpreter finalisation
// (Py_AtExit), once, through the diagnostic handler the module installed last
// (after step 5 the default one, writing to stderr, unless Python set None,
// which discards the report): the number of records refused in step 1, the
// number not delivered to Python sinks from step 2 on (and whether a wait was
// interrupted), an unfinished flush (step 3), and a callback still running.
// Nothing is reported when nothing was lost.
//
// After the hook no C++ thread calls into Python again; sinks written in C++
// keep working until static destruction. This also covers an unhandled
// exception and sys.exit(), which run atexit functions too. os._exit() and a
// fatal signal skip both atexit and C++ static destruction, so neither runs.

namespace lumen::python {

// How long the exit hook lets records reach Python sinks (step 2 above).
inline constexpr std::chrono::milliseconds exit_delivery_limit{1000};

// How long it then waits for Python callbacks still running (steps 3 and 6).
inline constexpr std::chrono::milliseconds exit_callback_limit{1000};

// What a PythonCall is for. A SinkCallback is refused from step 2 of the exit
// hook on; any other call (releasing a Python object, a diagnostic handler)
// until step 6.
enum class PythonCallKind : uint8_t {
    SinkCallback,
    Other
};

// Admission of one call into Python from a C++ thread that may not hold the
// GIL. Create one before acquiring the GIL; if admitted() is false, the call
// must be skipped: Python sinks are no longer called (exit hook step 2), or
// Python can no longer run code (step 6 ran, or the interpreter is not
// initialised or is finalising). The exit hook waits for every admitted
// PythonCall to be destroyed.
class PythonCall {
public:
    explicit PythonCall(PythonCallKind kind) noexcept;
    ~PythonCall();
    PythonCall(const PythonCall&) = delete;
    PythonCall& operator=(const PythonCall&) = delete;
    PythonCall(PythonCall&&) = delete;
    PythonCall& operator=(PythonCall&&) = delete;

    [[nodiscard]] bool admitted() const noexcept { return admitted_; }

private:
    bool admitted_;
};

// Whether a producer called from Python may submit its record. False from
// step 1 of the exit hook on; the refused record is counted for the report.
[[nodiscard]] bool admit_python_record() noexcept;

// Counts a record that a Python sink did not receive because its callback
// was refused (PythonCall not admitted), for the report.
void count_undelivered_python_record() noexcept;

// Registers a sink whose callbacks run Python code with `core`, so that the
// exit hook detaches it. Called with the GIL held; releases it around the
// registration. Raises RuntimeError once the exit hook has run, and the
// library's error for a null sink.
[[nodiscard]] SinkId add_python_sink(Core& core, std::unique_ptr<Sink> sink, Predicate predicate);

// Forgets a sink that was removed from core() (no-op for other ids).
void forget_python_sink(SinkId id) noexcept;

// Core::set_diagnostic_handler on `core`, recording the handler (for the
// exit report) and whether it calls Python, so that the exit hook knows to
// put the default one back. Called with the GIL held, which it keeps
// (destroying the previous handler may need it). Raises RuntimeError for a
// handler that calls Python once the exit hook has run.
void set_diagnostic_handler(Core& core, const DiagnosticHandler& handler, bool calls_python);

}  // namespace lumen::python

#endif
