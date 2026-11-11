#ifndef LUMEN_PYTHON_REGISTRATION_HPP
#define LUMEN_PYTHON_REGISTRATION_HPP

#include <pybind11/pybind11.h>

// The parts of the Python module, one function per area. python/module.cpp
// calls them in the order listed, which is the order their types depend on
// each other.

namespace lumen::python {

// The import name of the module (PYBIND11_MODULE in python/module.cpp).
inline constexpr const char* module_name = "lumen_bindings";

// LumenError and its subclasses (python/errors.cpp).
void register_errors(pybind11::module_& m);

// LogLevel, SourceLocation and the three read-only record types (python/records.cpp).
void register_records(pybind11::module_& m);

// Predicate and its builders (python/predicates.cpp).
void register_predicates(pybind11::module_& m);

// Sink (the base class Python sinks derive from) and CallbackSink (python/python_sinks.cpp).
void register_python_sinks(pybind11::module_& m);

// The library's own sinks and their configurations (python/sinks.cpp).
void register_sinks(pybind11::module_& m);

// Core, its statistics and core() (python/core_bindings.cpp).
void register_core(pybind11::module_& m);

// trace() ... fatal(), log(), metric(), progress() and Progress (python/producers.cpp).
void register_producers(pybind11::module_& m);

// Thread and process tags and scope() (python/context_bindings.cpp).
void register_context(pybind11::module_& m);

// The atexit hook that detaches Python sinks before the interpreter
// finalises (python/interpreter_exit.cpp). atexit runs its functions in
// reverse order of registration, so this one, registered at import, runs
// after every atexit function the program registers later.
void register_interpreter_exit(pybind11::module_& m);

}  // namespace lumen::python

#endif
