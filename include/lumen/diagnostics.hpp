#ifndef LUMEN_DIAGNOSTICS_HPP
#define LUMEN_DIAGNOSTICS_HPP

#include <functional>
#include <string_view>

// Where Lumen reports problems it cannot return to anyone: a Core (see
// Core::set_diagnostic_handler in lumen/core.hpp) and each shipped sink (its
// Config::diagnostics) take a DiagnosticHandler. Each of those headers
// states which lines its handler receives and on which thread.

namespace lumen {

// Receives one line of text about a problem. An empty handler discards the
// lines. A handler should not throw; what happens when one does, and on
// which threads it is called, is stated where it is installed.
using DiagnosticHandler = std::function<void(std::string_view line)>;

// The handler a Core and the shipped sinks start with: writes each line to
// std::cerr.
[[nodiscard]] DiagnosticHandler stderr_diagnostics();

}  // namespace lumen

#endif
