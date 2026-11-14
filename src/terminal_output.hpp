#ifndef LUMEN_SRC_TERMINAL_OUTPUT_HPP
#define LUMEN_SRC_TERMINAL_OUTPUT_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>

#include "line_writer.hpp"
#include "lumen/terminal_style.hpp"

// The destination of TerminalSink and DashboardSink: the caller's stream or
// standard error, and what can be learnt about it.

namespace lumen::detail {

// A terminal window's size in character cells.
struct TerminalSize {
    uint16_t columns = 0;
    uint16_t rows = 0;
};

class OutputStream {
public:
    // `stream` null: the process's standard error (std::cerr).
    explicit OutputStream(std::shared_ptr<std::ostream> stream) noexcept;

    [[nodiscard]] std::ostream& stream() const noexcept { return *stream_; }
    // Only standard error can be recognised as a terminal; a caller's stream
    // is never one.
    [[nodiscard]] bool is_terminal() const noexcept { return is_terminal_; }
    // Whether this is the process's standard error (no caller's stream).
    [[nodiscard]] bool is_standard_error() const noexcept { return !owned_; }
    // The size of the terminal this stream writes to, measured now: only
    // standard error can be measured (TIOCGWINSZ on POSIX, the console
    // window on Windows). nullopt for a caller's stream, or when standard
    // error is not a terminal.
    [[nodiscard]] std::optional<TerminalSize> terminal_size() const noexcept;

private:
    std::shared_ptr<std::ostream> owned_;  // keeps the caller's stream alive
    std::ostream* stream_;                 // non-owning: owned_.get() or &std::cerr
    bool is_terminal_;
};

// Whether a sink with this mode writing to `output` uses colour (see
// ColorMode::Auto for the rules).
[[nodiscard]] bool resolve_color(ColorMode mode, const OutputStream& output) noexcept;

// Writes the pieces and flushes the stream. Returns a description of the
// failure if the stream reported an error or threw (a stream with
// exceptions() enabled); in both cases the stream's error state is cleared,
// so that the next write is tried again.
[[nodiscard]] std::optional<std::string> write_text(
    std::ostream& stream, std::span<const std::string_view> pieces) noexcept;

// write_text for a batch of lines; a failure counts every line as failed.
[[nodiscard]] BatchOutcome write_and_flush(std::ostream& stream,
                                           std::span<const std::string> lines);

}  // namespace lumen::detail

#endif
