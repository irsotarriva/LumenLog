#ifndef LUMEN_SRC_TERMINAL_OUTPUT_HPP
#define LUMEN_SRC_TERMINAL_OUTPUT_HPP

#include <memory>
#include <ostream>
#include <span>
#include <string>

#include "line_writer.hpp"
#include "lumen/terminal_style.hpp"

// The destination of TerminalSink and DashboardSink: the caller's stream or
// standard error, and what can be learnt about it.

namespace lumen::detail {

class OutputStream {
public:
    // `stream` null: the process's standard error (std::cerr).
    explicit OutputStream(std::shared_ptr<std::ostream> stream) noexcept;

    [[nodiscard]] std::ostream& stream() const noexcept { return *stream_; }
    // Only standard error can be recognised as a terminal; a caller's stream
    // is never one.
    [[nodiscard]] bool is_terminal() const noexcept { return is_terminal_; }

private:
    std::shared_ptr<std::ostream> owned_;  // keeps the caller's stream alive
    std::ostream* stream_;                 // non-owning: owned_.get() or &std::cerr
    bool is_terminal_;
};

// Whether a sink with this mode writing to `output` uses colour (see
// ColorMode::Auto for the rules).
[[nodiscard]] bool resolve_color(ColorMode mode, const OutputStream& output) noexcept;

// Writes the lines as one block and flushes the stream. If the stream fails,
// counts every line as failed and clears the stream's error state, so that
// the next batch is tried again.
[[nodiscard]] BatchOutcome write_and_flush(std::ostream& stream,
                                           std::span<const std::string> lines);

}  // namespace lumen::detail

#endif
